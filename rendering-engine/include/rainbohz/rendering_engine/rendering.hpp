// SPDX-License-Identifier: Apache-2.0

#pragma once

// Rendering: the samples of each requested frame, from the partials in scope
// for it, spread over the output channels by each SoundSource's position and
// written as a multi-channel WAV file per frame (docs/rendering.md).
//
// Any frame can be rendered on its own: nothing rendered before a frame is
// needed to render it, because precomputation has made every partial's phase
// a function of time. So a run renders each requested frame in turn, and the
// frames do not interact. Within a frame, the partial sets are rendered on
// several threads (ADR 0026).
//
// This is the only piece of rendering that reads engine-db, through the
// FrameSource its backend provides (frame_gather.hpp), and it only reads. Per frame it gathers the
// frame's FrameEntry nodes and what follows them along the lists, validates each partial and its
// source (section 9), and hands the result to frame_plan.hpp, which does everything after that over
// in-memory data. Errors are exceptions, thrown at gather and caught per partial; a frame's failure
// is a consequence recorded in the report, not a second exception. Nothing on the render path after
// the gather throws.

#include "rainbohz/rendering_engine/channel_mode.hpp"
#include "rainbohz/rendering_engine/frame_gather.hpp"
#include "rainbohz/rendering_engine/render_kernel.hpp"
#include "rainbohz/rendering_engine/render_request.hpp"
#include "rainbohz/rendering_engine/sample_value.hpp"
#include "rainbohz/rendering_engine/units.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace rainbohz::rendering_engine {

/// Thrown when rendering cannot run at all: the configuration every frame
/// needs is missing or unusable. Nothing has been written.
class RenderingError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Thrown when one partial cannot be rendered in one frame. renderFrames()
/// catches it, records it against the partial and the frame, and fails the
/// frame.
///
/// The message says what is wrong in terms of the data; it does not name the
/// partial, which the caller knows.
class PartialRenderError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// One partial that failed validation, and where it first failed.
struct PartialFailure {
    /// Partial.id.
    std::int64_t partialId = 0;

    /// Partial.name; nullopt when NULL.
    std::optional<std::string> partialName;

    /// The frame in which the failure was found.
    FrameNumber frame{0};

    /// Why.
    std::string reason;
};

/// What happened to one requested frame.
struct FrameOutcome {
    /// The frame.
    FrameNumber frame{0};

    /// Why the frame was not rendered; nullopt when it was. A failed frame
    /// has no file.
    std::optional<std::string> error;

    /// Partials that failed validation for the first time in this frame,
    /// with their reasons. Reported once each, in this frame.
    std::vector<PartialFailure> newFailures;

    /// How many SoundSources contributed. Zero for a silent frame.
    std::size_t sourceCount = 0;

    /// How many partial sets contributed. Zero for a silent frame.
    std::size_t setCount = 0;

    /// How many partials contributed.
    std::size_t partialCount = 0;

    /// The largest absolute sample value in the frame, over every channel.
    SampleValue peak = 0;

    /// The file written; empty on failure.
    std::filesystem::path file;

    /// With --per-set-wav, each contributing set's id and file, in set order.
    std::vector<std::pair<std::int64_t, std::filesystem::path>> setFiles;

    /// Each partial whose phase the kernel held at least once in this frame,
    /// because a one-sample step exceeded kMaxPhaseStep or was not finite
    /// (render_kernel.hpp), with how many times. Partial id order. Empty for
    /// any frame whose frequencies stay within the limit, which is the norm.
    std::vector<std::pair<std::int64_t, std::int64_t>> cappedPhases;

    /// True when the frame was rendered and its file written.
    ///
    /// @return Whether `error` is unset.
    bool ok() const noexcept { return !error.has_value(); }
};

/// The result of a rendering run.
struct RenderingReport {
    /// EngineConfig.sampleRate.
    SampleRate sampleRate{0};

    /// The frame size every frame was rendered with.
    FrameSize frameSize{0};

    /// EngineConfig.channelMode: how many channels each frame's file has,
    /// and what they are.
    ChannelMode channelMode = ChannelMode::Mono;

    /// How many threads rendered each frame's partial sets, resolved
    /// (resolveThreadCount()). Says nothing about the output, which is the
    /// same for every count.
    std::size_t threads = 1;

    /// Which kernel rendered the partial sets (ADR 0027).
    RenderKernel kernel = RenderKernel::Simd;

    /// One outcome per requested frame, in request order.
    std::vector<FrameOutcome> frames;

    /// How many frames failed.
    ///
    /// @return The number of entries in `frames` whose error is set.
    std::size_t failureCount() const noexcept;

    /// How many partials failed validation, each counted once.
    ///
    /// @return The number of newFailures over every frame.
    std::size_t failedPartialCount() const noexcept;

    /// True when every frame was rendered, including when none was requested.
    ///
    /// @return Whether failureCount() is zero.
    bool ok() const noexcept { return failureCount() == 0; }
};

/// The name of a frame's file: `frame-000003.wav`, zero-padded to six digits
/// so that files list in order.
///
/// @param frame  The frame.
/// @return The file name, with no directory.
std::string frameFileName(FrameNumber frame);

/// The name of a partial set's file under --per-set-wav:
/// `frame-000003.set-000002.wav`.
///
/// @param frame         The frame.
/// @param partialSetId  PartialSet.id, which is stable within a run where
///                      names may be NULL or repeated.
/// @return The file name, with no directory.
std::string setFileName(FrameNumber frame, std::int64_t partialSetId);

/// Renders every frame of `request` (docs/rendering.md section 6.1).
///
/// Reads the EngineConfig once, then for each requested frame in request
/// order: gathers the frame's inputs through its FrameEntry nodes (section
/// 6.3), validates every partial in scope and its SoundSource's spatial
/// timelines (section 9), renders each partial set into its own mono buffer
/// (frame_plan.hpp) on up to `threads` threads, sums each source's sets in
/// id order on the calling thread, spreads each source's sum over
/// the output channels by its position (section 8.5), and writes
/// `outputDir / frameFileName(k)` with one channel per output of the
/// channel mode. Each frame's file is complete before the
/// next starts. A frame with nothing in scope, including one for which no
/// Frame node exists, is written as a full-length file of zeros.
///
/// A frame in which any partial fails validation is not rendered and has no
/// file. A partial that fails is reported in the frame it first failed in;
/// later frames that fail because of it name it without repeating the
/// reason. A file that cannot be written fails its frame the same way, and
/// any file written for that frame is removed.
///
/// Labels in the request are ignored in this version: every partial in a
/// frame's scope is rendered (section 2).
///
/// @param[in,out] source  engine-db holding the precomputed score, as
///                   rendering reads it. Read only.
/// @param request    The frames to render.
/// @param outputDir  Where the files go. Must exist.
/// @param perSetWav  Whether to also write each contributing partial set's
///                   buffer, before summing and spatialisation, as
///                   `setFileName()`. These files are always mono.
/// @param threads    Threads rendering a frame's partial sets, the caller's
///                   included; 0 for one per hardware thread
///                   (resolveThreadCount()). The files and the report are
///                   byte-for-byte the same for every count (ADR 0026): only
///                   the time taken changes. 1, the default, creates no
///                   thread.
/// @param kernel     Which kernel renders the chunks: SIMD, the default, or
///                   the scalar reference, whose output the SIMD kernel's
///                   agrees with within the bound of section 10.
/// @return One outcome per requested frame, in request order.
/// @throws RenderingError  If there is not exactly one EngineConfig; its
///         sampleRate is NULL or not positive; its frame size is unusable;
///         or its channelMode is NULL or not 0 to 7. Nothing has been
///         written.
/// @throws EngineDbError  If reading the configuration fails. A database
///         failure while gathering a frame fails that frame instead.
/// @note Side effect: writes WAV files into `outputDir`, replacing any of the
///       same names.
RenderingReport renderFrames(FrameSource& source, const RenderRequest& request,
    const std::filesystem::path& outputDir, bool perSetWav, std::size_t threads = 1,
    RenderKernel kernel = RenderKernel::Simd);

} // namespace rainbohz::rendering_engine
