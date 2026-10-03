#include "rainbohz/playback_client/buffer_dir.hpp"
#include "rainbohz/playback_client/object_fetch.hpp"
#include "rainbohz/playback_client/wav_io.hpp"

#include "test_support.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <vector>

namespace rainbohz::playback_client {
namespace {

using test::TempDir;

// --- file:// URIs ------------------------------------------------------------

TEST(FileUri, DecodesWhatTheEngineWrites) {
    EXPECT_EQ(fileUriToPath("file:///tmp/frame-000001.wav"),
        std::filesystem::path{"/tmp/frame-000001.wav"});
    EXPECT_EQ(fileUriToPath("file:///a%20b/c%25d"), std::filesystem::path{"/a b/c%d"});
    EXPECT_EQ(fileUriToPath("FILE://LocalHost/x"), std::filesystem::path{"/x"});
}

TEST(FileUri, RefusesMalformedUris) {
    for (const char* uri : {"http://x/y", "file:/x", "file://host/x", "file://", "file:///x?q",
             "file:///x#f", "file:///x%2", "file:///x%zz", "file:///x%00"}) {
        EXPECT_THROW(fileUriToPath(uri), ObjectFetchError) << uri;
    }
}

TEST(ObjectFetcher, CopiesAFileObject) {
    const TempDir dir;
    const auto source = dir.path() / "source.bin";
    std::ofstream{source} << "hello";
    const auto fetcher = makeObjectFetcher();
    fetcher->fetch("file://" + source.string(), dir.path() / "copy.bin");
    std::ifstream copy{dir.path() / "copy.bin"};
    std::string text;
    copy >> text;
    EXPECT_EQ(text, "hello");
}

TEST(ObjectFetcher, RefusesOtherSchemes) {
    const TempDir dir;
    const auto fetcher = makeObjectFetcher();
    EXPECT_THROW(fetcher->fetch("s3://bucket/frame.wav", dir.path() / "x"), ObjectFetchError);
    EXPECT_THROW(fetcher->fetch("https://h/frame.wav", dir.path() / "x"), ObjectFetchError);
}

TEST(ObjectFetcher, ReportsAMissingObject) {
    const TempDir dir;
    const auto fetcher = makeObjectFetcher();
    EXPECT_THROW(fetcher->fetch("file://" + (dir.path() / "absent.wav").string(), dir.path() / "x"),
        ObjectFetchError);
}

// --- Buffer directory --------------------------------------------------------

TEST(BufferDir, MakesAUniqueRunDirectoryWithTwoLanes) {
    const TempDir root;
    const BufferDir first{root.path()};
    const BufferDir second{root.path()};
    EXPECT_NE(first.path(), second.path());
    EXPECT_EQ(first.path().parent_path(), root.path());
    EXPECT_TRUE(first.path().filename().string().starts_with("rainbohz-playback-"));
    EXPECT_TRUE(std::filesystem::is_directory(first.laneDir(Lane::A)));
    EXPECT_TRUE(std::filesystem::is_directory(first.laneDir(Lane::B)));
    EXPECT_EQ(
        first.framePath(Lane::B, FrameNumber{3}), first.path() / "lane-b" / "frame-000003.wav");
    EXPECT_EQ(
        first.partPath(Lane::A, FrameNumber{3}), first.path() / "lane-a" / "frame-000003.wav.part");
}

TEST(BufferDir, RemovesOnlyItsOwnDirectory) {
    const TempDir root;
    const BufferDir buffer{root.path()};
    std::ofstream{buffer.framePath(Lane::A, FrameNumber{0})} << "x";
    buffer.remove();
    EXPECT_FALSE(std::filesystem::exists(buffer.path()));
    EXPECT_TRUE(std::filesystem::is_directory(root.path()));
}

TEST(BufferDir, RefusesARootThatIsNotADirectory) {
    EXPECT_THROW(BufferDir{"/no/such/root"}, BufferDirError);
}

TEST(Lanes, AlternateByLoop) {
    EXPECT_EQ(laneForLoop(0), Lane::A);
    EXPECT_EQ(laneForLoop(1), Lane::B);
    EXPECT_EQ(laneForLoop(2), Lane::A);
    EXPECT_EQ(otherLane(Lane::A), Lane::B);
    EXPECT_EQ(laneName(Lane::B), "b");
}

// --- WAV ---------------------------------------------------------------------

TEST(Wav, ReadsBackWhatItWrote) {
    const TempDir dir;
    const auto file = dir.path() / "f.wav";
    const std::vector<float> samples{0.5F, -0.25F, 1.0F, 0.0F};
    {
        WavWriter writer{file, SampleRate{8000}};
        writer.write(samples);
        writer.close();
    }
    EXPECT_EQ(readFrame(file, SampleRate{8000}, FrameSize{4}), samples);
}

TEST(Wav, RefusesAFrameOfTheWrongRateOrLength) {
    const TempDir dir;
    const auto file = dir.path() / "f.wav";
    {
        WavWriter writer{file, SampleRate{8000}};
        writer.write(std::vector<float>(4, 0.0F));
        writer.close();
    }
    EXPECT_THROW(readFrame(file, SampleRate{44100}, FrameSize{4}), WavError);
    EXPECT_THROW(readFrame(file, SampleRate{8000}, FrameSize{5}), WavError);
    EXPECT_THROW(readFrame(dir.path() / "absent.wav", SampleRate{8000}, FrameSize{4}), WavError);
}

} // namespace
} // namespace rainbohz::playback_client
