// SPDX-License-Identifier: Apache-2.0

#pragma once

// The batch-mode render request: which frames to render, and for each, which
// labels select the partials to render.
//
// docs/design.md section 3 says a request names a frame, a label set and a
// source version. In batch mode the source version is implicit -- it is
// whatever the --state file loaded -- so the file carries the other two:
//
//   frames:
//     - frame: 1
//       labels: [strings, brass]   # only partials carrying these labels
//     - frame: 2                   # no labels: every partial
//
// A request is checked in two stages. Parsing (parseRenderRequest) checks the
// file's shape and needs nothing but the text. Label resolution
// (findUnknownLabels) needs the labels engine-db actually holds, so it runs
// once the score is loaded. Both report every problem they find, each with its
// source line, rather than stopping at the first -- the same policy the
// Cypher loader follows.
//
// yaml-cpp is confined to the implementation: no YAML type appears here.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace rainbohz::rendering_engine {

/// One frame to render.
struct FrameRequest {
    /// 0 or greater: frame 0 is the first frame of the score. INT32, because
    /// that is the type of Frame.frame_number in
    /// engine-db/schema/engine-schema.cypher.
    std::int32_t frameNumber = 0;

    /// The labels that select which partials to render. Empty means every
    /// partial: an omitted `labels` key and `labels: []` are the same request.
    /// A set, because order carries no meaning and a repeat adds nothing.
    std::set<std::string> labels;

    /// The 1-based line of this entry in the request file, for diagnostics.
    std::size_t line = 0;

    /// Whether this entry renders every partial rather than a labelled subset.
    ///
    /// @return True when `labels` is empty, which is how both an omitted
    ///         `labels` key and `labels: []` are represented.
    bool allPartials() const noexcept { return labels.empty(); }
};

/// A whole request, as read from one file.
struct RenderRequest {
    /// Where the request came from, for diagnostics.
    std::filesystem::path file;

    /// In file order. No two entries share a frame number.
    std::vector<FrameRequest> frames;
};

/// One thing wrong with a request, attributed to a line.
struct RequestProblem {
    /// 1-based. 0 when the problem belongs to the file as a whole.
    std::size_t line = 0;
    std::string message;
};

/// Thrown when a request cannot be used: it is unreadable, is not YAML, or
/// does not have the request's shape. Carries every problem found, not only
/// the first.
class RenderRequestError : public std::runtime_error {
public:
    /// Builds the error for `file` from the problems found in it.
    ///
    /// The what() text is a summary line naming the full path, followed by
    /// one indented line per problem as formatted by describeProblem(), so
    /// that printing what() alone reports everything.
    ///
    /// @param file      The request file the problems belong to. Kept, and
    ///                  used in full in the summary line.
    /// @param problems  Everything found wrong, in the order it was found.
    ///                  Expected to be non-empty; an empty list gives an error
    ///                  that states the file is unusable without saying why.
    RenderRequestError(std::filesystem::path file, std::vector<RequestProblem> problems);

    /// The request file the problems belong to.
    ///
    /// @return The path as given to the constructor. The reference is valid
    ///         for the lifetime of this exception object.
    const std::filesystem::path& file() const noexcept { return file_; }

    /// Everything found wrong with the request.
    ///
    /// For callers that want to report the problems in their own format
    /// rather than use what().
    ///
    /// @return The problems as given to the constructor, in the order they
    ///         were found. The reference is valid for the lifetime of this
    ///         exception object.
    const std::vector<RequestProblem>& problems() const noexcept { return problems_; }

private:
    std::filesystem::path file_;
    std::vector<RequestProblem> problems_;
};

/// Formats one problem as a single diagnostic line.
///
/// Only the file's name is shown, not its directory: the full path is given
/// once, in the summary line of RenderRequestError::what(), and repeating it
/// on every line would bury the messages.
///
/// @param file     The request file the problem belongs to. Only its final
///                 component is used.
/// @param problem  The problem to describe.
/// @return "request.yaml, line 4: <message>", or "request.yaml: <message>"
///         when `problem.line` is 0 (the problem belongs to the file as a
///         whole). No trailing newline.
std::string describeProblem(const std::filesystem::path& file, const RequestProblem& problem);

/// Parses a render request from YAML text, checking its shape.
///
/// This is the first of the two checking stages described at the top of this
/// file: it needs nothing but the text, and does not check that the labels
/// exist in engine-db (see findUnknownLabels()). The text must be exactly one
/// YAML document: a mapping whose only key is `frames`, holding a non-empty
/// list of entries, each a mapping with a `frame` key and optionally a
/// `labels` key. Frame numbers must be plain, unquoted decimal integers from 0
/// to the INT32 maximum, and no two entries may name the same frame. Keys are
/// case-sensitive, and an unknown or repeated key is an error rather than
/// being ignored.
///
/// Every problem is collected before anything is thrown, so one error lists
/// all of them, each with its line.
///
/// @param yaml  The complete request text. Copied: the view need not outlive
///              the call.
/// @param file  Where the text came from. Used only to label diagnostics and
///              to fill RenderRequest::file; nothing is read from it.
/// @return The request, with its frame entries in file order.
/// @throws RenderRequestError  If the text is not valid YAML (a single
///                             problem, carrying yaml-cpp's message and
///                             position); is empty; holds more than one YAML
///                             document; or is not a request of the shape
///                             above (every such problem found).
RenderRequest parseRenderRequest(std::string_view yaml, const std::filesystem::path& file);

/// Reads a request file and parses it with parseRenderRequest().
///
/// The whole file is read into memory before parsing. The bytes are passed
/// through unchanged (the file is opened in binary mode); line-ending handling
/// is left to yaml-cpp.
///
/// @param file  Path of the request file.
/// @return The parsed request, with RenderRequest::file set to `file`.
/// @throws RenderRequestError  If `file` does not exist, is not a regular
///                             file, or cannot be opened for reading; and for
///                             everything parseRenderRequest() rejects.
/// @note Side effect: reads `file` from disk.
RenderRequest loadRenderRequest(const std::filesystem::path& file);

/// Finds every label the request names that engine-db does not hold.
///
/// This is the second of the two checking stages described at the top of this
/// file, run once the score is loaded and the labels it defines are known.
///
/// An unknown label is an error rather than a label that selects nothing: a
/// misspelt label would otherwise render silence, which looks like success.
/// Problems are returned rather than thrown so that the caller can decide how
/// to report them alongside its own diagnostics.
///
/// @param request      A request that has passed parseRenderRequest().
/// @param knownLabels  Every label engine-db holds. Matching is exact and
///                     case-sensitive.
/// @return One problem for each (frame entry, unknown label) pair, attributed
///         to the entry's line: a label misspelt in three entries is reported
///         three times. Ordered by entry in file order, then by label.
///         Empty when every label is known, including when no entry names any
///         labels.
std::vector<RequestProblem> findUnknownLabels(
    const RenderRequest& request, const std::set<std::string>& knownLabels);

} // namespace rainbohz::rendering_engine
