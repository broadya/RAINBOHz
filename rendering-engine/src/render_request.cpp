#include "rainbohz/rendering_engine/render_request.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

/// @file
/// @brief Parses and checks the test-mode YAML render request.
///
/// Implements parseRenderRequest(), loadRenderRequest(), findUnknownLabels()
/// and describeProblem(). yaml-cpp is confined to this file.
///
/// Parsing walks the document with a Checker that collects every problem,
/// each with its source line, instead of stopping at the first. At the end the
/// problems are thrown together as one RenderRequestError. The format is read
/// strictly: an unknown or repeated key, a quoted or non-decimal frame number,
/// a frame beyond INT32, a repeated frame and a second YAML document are all
/// refused rather than reinterpreted. Label resolution runs separately, once
/// engine-db is loaded, against the labels it actually holds.
///
/// @see render_request.hpp

namespace rainbohz::rendering_engine {

namespace {

constexpr std::string_view kFramesKey = "frames";
constexpr std::string_view kFrameKey = "frame";
constexpr std::string_view kLabelsKey = "labels";

/// yaml-cpp's tag for a plain (unquoted) scalar. A quoted scalar is tagged "!",
/// which is how `frame: "3"` is told apart from `frame: 3`.
constexpr std::string_view kPlainScalarTag = "?";

/// The line a node starts on, for a diagnostic.
///
/// For an empty value this is not the line of its key; use Value::line() for
/// that.
///
/// @param node  The node to locate.
/// @return The 1-based line, or 0 if yaml-cpp does not know it (its mark
///         line is negative).
std::size_t lineOf(const YAML::Node& node) {
    const int line = node.Mark().line;
    return line < 0 ? 0 : static_cast<std::size_t>(line) + 1;
}

/// The line of a position yaml-cpp reports, for a diagnostic.
///
/// Used for the position carried by a YAML::Exception, which has a mark but
/// no node.
///
/// @param mark  The position to convert. yaml-cpp counts lines from 0.
/// @return The 1-based line, or 0 if the mark's line is negative (unknown).
std::size_t lineOf(const YAML::Mark& mark) {
    return mark.line < 0 ? 0 : static_cast<std::size_t>(mark.line) + 1;
}

/// A value in a mapping, with the line of the key that introduced it.
///
/// An empty value (`frame:` with nothing after it) has no text of its own, and
/// yaml-cpp places it at the next token -- which is on the following line, or
/// past the end of the file. The key's line is where the problem is.
///
/// yaml-cpp's Node has no noexcept move, so neither does this. That is safe --
/// containers fall back to copying -- and there is nothing to be done about it
/// short of wrapping Node.
// NOLINTNEXTLINE(bugprone-exception-escape)
struct Value {
    YAML::Node node;
    std::size_t keyLine = 0;

    /// The line to attribute a problem with this value to.
    ///
    /// @return `keyLine` when the value is empty (null), since yaml-cpp's own
    ///         position for it is misleading; otherwise the value's own line.
    ///         Either may be 0 if the line is not known.
    std::size_t line() const { return node.IsNull() ? keyLine : lineOf(node); }
};

/// Describes a node for quoting in a diagnostic, as in "..., not 'x'".
///
/// Only scalars have text of their own; anything else is described by kind,
/// so that a diagnostic never dumps a whole list or mapping.
///
/// @param node  The offending node.
/// @return The scalar's text in single quotes (unescaped, so a value that
///         itself contains a quote is shown as-is); otherwise "a list",
///         "a mapping", or "nothing" for a null or undefined node.
std::string quoted(const YAML::Node& node) {
    if (node.IsScalar()) {
        return "'" + node.Scalar() + "'";
    }
    if (node.IsSequence()) {
        return "a list";
    }
    if (node.IsMap()) {
        return "a mapping";
    }
    return "nothing";
}

/// Collects problems as the parser walks the document.
///
/// Parsing functions report to a Checker and carry on rather than throwing,
/// so that one pass finds every problem. The caller decides at the end, with
/// clean(), whether to throw.
class Checker {
public:
    /// Records a problem.
    ///
    /// @param line     1-based line the problem is attributed to, or 0 when it
    ///                 belongs to the file as a whole.
    /// @param message  What is wrong, phrased to follow "request.yaml, line N: ".
    ///                 Moved in.
    void report(std::size_t line, std::string message) {
        problems_.push_back(RequestProblem{.line = line, .message = std::move(message)});
    }

    /// Whether anything has been reported.
    ///
    /// @return True when no problem has been reported since construction.
    bool clean() const noexcept { return problems_.empty(); }

    /// Hands over the problems collected so far.
    ///
    /// Call once, when the walk is finished: the Checker is left moved-from
    /// and should not be reported to afterwards.
    ///
    /// @return Every problem, in the order reported.
    std::vector<RequestProblem> take() { return std::move(problems_); }

    /// Collects the permitted keys of a mapping, reporting the rest.
    ///
    /// yaml-cpp does not reject a repeated key -- a lookup simply finds the
    /// first -- so a request that says `frame:` twice would otherwise lose one
    /// silently. Reading the keys this way also makes a misspelt key an error
    /// rather than an ignored one.
    ///
    /// Only the keys present are checked; a required key that is missing is
    /// for the caller to report, since only the caller knows which are
    /// required.
    ///
    /// @param map      The mapping to read. Must be a map node.
    /// @param allowed  Every key the mapping may have. Matching is exact and
    ///                 case-sensitive.
    /// @param where    The mapping as named in a diagnostic, such as "a frame
    ///                 entry", to follow "in".
    /// @return Each permitted key present, with its value and the key's line.
    ///         For a repeated key, the first occurrence.
    /// @note Side effect: reports each key that is not a scalar, is not in
    ///       `allowed`, or repeats one already seen, at that key's line.
    std::map<std::string, Value> keys(const YAML::Node& map,
        std::initializer_list<std::string_view> allowed, std::string_view where) {
        std::map<std::string, Value> found;
        for (const auto& entry : map) {
            const YAML::Node& key = entry.first;
            if (!key.IsScalar()) {
                report(lineOf(key), "keys in " + std::string{where} + " must be names");
                continue;
            }
            const std::string& name = key.Scalar();
            if (std::ranges::find(allowed, name) == allowed.end()) {
                report(lineOf(key), "unknown key '" + name + "' in " + std::string{where} +
                                        " (expected " + expected(allowed) + ")");
                continue;
            }
            if (!found.emplace(name, Value{.node = entry.second, .keyLine = lineOf(key)}).second) {
                report(lineOf(key), "key '" + name + "' appears twice in " + std::string{where});
            }
        }
        return found;
    }

private:
    /// Lists the permitted keys for an "unknown key" diagnostic.
    ///
    /// @param allowed  The permitted keys, in the order to list them.
    /// @return The keys, each in single quotes, separated by ", ": for
    ///         example "'frame', 'labels'". Empty when `allowed` is.
    static std::string expected(std::initializer_list<std::string_view> allowed) {
        std::string names;
        for (const std::string_view name : allowed) {
            names += names.empty() ? "" : ", ";
            names += "'" + std::string{name} + "'";
        }
        return names;
    }

    std::vector<RequestProblem> problems_;
};

/// Parses the value of a `frame` key into a frame number.
///
/// A frame number is a plain decimal integer, 0 or greater, that fits the
/// schema's INT32. Frames are indexed from 0 -- frame 0 is the first second of
/// the score -- because a frame is an internal index, never shown to a user.
/// Hex, octal, signs, exponents and quoted numbers are all refused, so that
/// what the file says is exactly the frame that is rendered. Leading zeros
/// are accepted and read as decimal (`007` is frame 7).
///
/// @param value        The value of the `frame` key, with its key's line.
/// @param[in,out] checker  Receives exactly one problem when the value is
///                     refused, and nothing otherwise.
/// @return The frame number, or std::nullopt when the value is not an
///         unquoted scalar, is not all decimal digits, or exceeds the INT32
///         maximum.
std::optional<std::int32_t> parseFrameNumber(const Value& value, Checker& checker) {
    const YAML::Node& node = value.node;
    const std::string rule = "frame must be an integer, 0 or greater";
    if (!node.IsScalar() || node.Tag() != kPlainScalarTag) {
        checker.report(value.line(),
            rule + ", not " + (node.IsScalar() ? "the quoted text " : "") + quoted(node));
        return std::nullopt;
    }

    const std::string& text = node.Scalar();
    const bool digitsOnly = !text.empty() && std::ranges::all_of(text, [](char character) {
        return std::isdigit(static_cast<unsigned char>(character)) != 0;
    });
    if (!digitsOnly) {
        checker.report(lineOf(node), rule + ", not " + quoted(node));
        return std::nullopt;
    }

    std::int32_t number = 0;
    const char* const last = std::to_address(text.cend());
    const auto [end, error] = std::from_chars(text.data(), last, number);
    if (error == std::errc::result_out_of_range) {
        checker.report(lineOf(node), "frame " + text + " is too large (the largest is " +
                                         std::to_string(std::numeric_limits<std::int32_t>::max()) +
                                         ")");
        return std::nullopt;
    }
    if (error != std::errc{} || end != last || number < 0) {
        checker.report(lineOf(node), rule + ", not " + quoted(node));
        return std::nullopt;
    }
    return number;
}

/// Parses the value of a `labels` key into a label set.
///
/// An absent or null `labels` is the empty set, meaning every partial. Labels
/// are case-sensitive names; whether they exist in engine-db is checked
/// later, by findUnknownLabels(). A label repeated in one list is kept once,
/// without comment, since repeating it changes nothing.
///
/// @param value        The value of the `labels` key, with its key's line.
/// @param[in,out] checker  Receives one problem if the value is not a list,
///                     otherwise one for each element that is not a non-empty
///                     scalar.
/// @return The valid labels. When the value is not a list, the empty set;
///         the reported problem makes the request fail, so this never
///         becomes an every-partial request by mistake.
std::set<std::string> parseLabels(const Value& value, Checker& checker) {
    const YAML::Node& node = value.node;
    std::set<std::string> labels;
    if (!node.IsDefined() || node.IsNull()) {
        return labels;
    }
    if (!node.IsSequence()) {
        // `labels: strings` is the likely mistake, so say what to write.
        checker.report(
            lineOf(node), "labels must be a list, not " + quoted(node) +
                              (node.IsScalar() ? " (write [" + node.Scalar() + "])" : ""));
        return labels;
    }
    for (const YAML::Node& label : node) {
        if (!label.IsScalar() || label.Scalar().empty()) {
            checker.report(lineOf(label), "a label must be a non-empty name, not " + quoted(label));
            continue;
        }
        labels.insert(label.Scalar());
    }
    return labels;
}

/// Parses one entry of the `frames` list.
///
/// Every problem in the entry is reported, including those in `labels` when
/// the frame number is itself bad.
///
/// @param entry        One element of the `frames` sequence.
/// @param[in,out] checker  Receives every problem found in the entry: a
///                     non-mapping entry, unknown or repeated keys, a missing
///                     or invalid `frame`, and bad `labels`.
/// @return The entry, with `line` set to the entry's own line; or
///         std::nullopt when the entry is not a mapping or has no valid frame
///         number. An entry whose only problems are in `labels` is still
///         returned, so that the caller can check its frame number for
///         duplicates; the problems already reported fail the request.
std::optional<FrameRequest> parseFrameEntry(const YAML::Node& entry, Checker& checker) {
    if (!entry.IsMap()) {
        checker.report(lineOf(entry),
            "each entry in frames must be a mapping with a 'frame' key, not " + quoted(entry));
        return std::nullopt;
    }

    auto keys = checker.keys(entry, {kFrameKey, kLabelsKey}, "a frame entry");

    FrameRequest request;
    request.line = lineOf(entry);

    const auto frame = keys.find(std::string{kFrameKey});
    std::optional<std::int32_t> number;
    if (frame == keys.end()) {
        checker.report(request.line, "frame entry has no 'frame' key");
    } else {
        number = parseFrameNumber(frame->second, checker);
    }

    const auto labels = keys.find(std::string{kLabelsKey});
    if (labels != keys.end()) {
        request.labels = parseLabels(labels->second, checker);
    }

    if (!number.has_value()) {
        return std::nullopt;
    }
    request.frameNumber = *number;
    return request;
}

} // namespace

RenderRequestError::RenderRequestError(
    std::filesystem::path file, std::vector<RequestProblem> problems)
    : std::runtime_error{[&] {
          std::string text = "render request " + file.string() + " is not usable";
          for (const RequestProblem& problem : problems) {
              text += "\n  " + describeProblem(file, problem);
          }
          return text;
      }()},
      file_{std::move(file)}, problems_{std::move(problems)} {}

std::string describeProblem(const std::filesystem::path& file, const RequestProblem& problem) {
    std::string text = file.filename().string();
    if (problem.line != 0) {
        text += ", line " + std::to_string(problem.line);
    }
    return text + ": " + problem.message;
}

RenderRequest parseRenderRequest(std::string_view yaml, const std::filesystem::path& file) {
    std::vector<YAML::Node> documents;
    try {
        documents = YAML::LoadAll(std::string{yaml});
    } catch (const YAML::Exception& e) {
        throw RenderRequestError{file, {{.line = lineOf(e.mark), .message = e.msg}}};
    }

    if (documents.empty() || documents.front().IsNull()) {
        throw RenderRequestError{file, {{.line = 0, .message = "the request is empty"}}};
    }
    if (documents.size() > 1) {
        // yaml-cpp's Load would read the first document and drop the rest, so
        // a second one is refused rather than ignored.
        throw RenderRequestError{
            file, {{.line = lineOf(documents[1]),
                      .message = "a request is one YAML document; this file holds " +
                                 std::to_string(documents.size())}}};
    }

    const YAML::Node& root = documents.front();
    if (!root.IsMap()) {
        throw RenderRequestError{
            file, {{.line = lineOf(root),
                      .message =
                          "a request must be a mapping with a 'frames' key, not " + quoted(root)}}};
    }

    Checker checker;
    auto keys = checker.keys(root, {kFramesKey}, "the request");

    RenderRequest request;
    request.file = file;

    const auto frames = keys.find(std::string{kFramesKey});
    if (frames == keys.end()) {
        checker.report(lineOf(root), "the request has no 'frames' key");
    } else if (!frames->second.node.IsSequence()) {
        checker.report(
            frames->second.line(), "frames must be a list, not " + quoted(frames->second.node));
    } else if (frames->second.node.size() == 0) {
        checker.report(frames->second.line(), "frames is empty: the request renders nothing");
    } else {
        // Frame number -> the line of the entry that first named it. One frame
        // is one output file, so two entries for the same frame would collide.
        std::map<std::int32_t, std::size_t> seen;
        for (const YAML::Node& entry : frames->second.node) {
            auto frame = parseFrameEntry(entry, checker);
            if (!frame.has_value()) {
                continue;
            }
            const auto [previous, inserted] = seen.emplace(frame->frameNumber, frame->line);
            if (!inserted) {
                checker.report(frame->line, "frame " + std::to_string(frame->frameNumber) +
                                                " is already requested at line " +
                                                std::to_string(previous->second));
                continue;
            }
            request.frames.push_back(std::move(*frame));
        }
    }

    if (!checker.clean()) {
        throw RenderRequestError{file, checker.take()};
    }
    return request;
}

RenderRequest loadRenderRequest(const std::filesystem::path& file) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(file, error)) {
        throw RenderRequestError{file, {{.line = 0, .message = "not a readable file"}}};
    }
    const std::ifstream stream{file, std::ios::binary};
    if (!stream) {
        throw RenderRequestError{file, {{.line = 0, .message = "cannot be opened"}}};
    }
    std::ostringstream contents;
    contents << stream.rdbuf();
    return parseRenderRequest(contents.str(), file);
}

std::vector<RequestProblem> findUnknownLabels(
    const RenderRequest& request, const std::set<std::string>& knownLabels) {
    std::vector<RequestProblem> problems;
    for (const FrameRequest& frame : request.frames) {
        for (const std::string& label : frame.labels) {
            if (!knownLabels.contains(label)) {
                problems.push_back(RequestProblem{.line = frame.line,
                    .message = "frame " + std::to_string(frame.frameNumber) + " names label '" +
                               label + "', which engine-db does not hold"});
            }
        }
    }
    return problems;
}

} // namespace rainbohz::rendering_engine
