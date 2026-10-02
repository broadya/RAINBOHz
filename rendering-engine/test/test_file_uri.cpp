// Tests for the file:// URI conversions.
//
// The reading side decides which file the server opens, so its refusals are
// tested as carefully as its successes.

#include "rainbohz/rendering_engine/file_uri.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace {

using rainbohz::rendering_engine::FileUriError;
using rainbohz::rendering_engine::fileUriToPath;
using rainbohz::rendering_engine::pathToFileUri;

TEST(FileUri, AnAbsolutePathIsRead) {
    EXPECT_EQ(
        fileUriToPath("file:///scores/demo.cypher"), std::filesystem::path{"/scores/demo.cypher"});
}

TEST(FileUri, LocalhostIsTheSameAsNoHost) {
    EXPECT_EQ(fileUriToPath("file://localhost/scores/demo.cypher"),
        std::filesystem::path{"/scores/demo.cypher"});
    EXPECT_EQ(fileUriToPath("FILE://LocalHost/a"), std::filesystem::path{"/a"});
}

TEST(FileUri, PercentEncodingIsDecoded) {
    EXPECT_EQ(fileUriToPath("file:///a%20b/c%2Fd%e2%82%ac"),
        std::filesystem::path{"/a b/c/d\xE2\x82\xAC"});
}

TEST(FileUri, ThePathIsNotNormalised) {
    // Confining a path is the caller's job, after resolving it; this function
    // must not hide a `..` by tidying it away.
    EXPECT_EQ(fileUriToPath("file:///a/../b"), std::filesystem::path{"/a/../b"});
}

TEST(FileUri, OtherSchemesAreRefused) {
    EXPECT_THROW(fileUriToPath("http://host/a"), FileUriError);
    EXPECT_THROW(fileUriToPath("s3://bucket/a"), FileUriError);
    EXPECT_THROW(fileUriToPath("/plain/path"), FileUriError);
    EXPECT_THROW(fileUriToPath("file:/a"), FileUriError);
    EXPECT_THROW(fileUriToPath(""), FileUriError);
}

TEST(FileUri, ARemoteHostIsRefused) {
    EXPECT_THROW(fileUriToPath("file://server/share/a"), FileUriError);
}

TEST(FileUri, AMissingPathIsRefused) {
    EXPECT_THROW(fileUriToPath("file://"), FileUriError);
    EXPECT_THROW(fileUriToPath("file://localhost"), FileUriError);
}

TEST(FileUri, AQueryOrFragmentIsRefused) {
    EXPECT_THROW(fileUriToPath("file:///a?b=c"), FileUriError);
    EXPECT_THROW(fileUriToPath("file:///a#b"), FileUriError);
}

TEST(FileUri, BadPercentEncodingIsRefused) {
    EXPECT_THROW(fileUriToPath("file:///a%2"), FileUriError);
    EXPECT_THROW(fileUriToPath("file:///a%zz"), FileUriError);
    EXPECT_THROW(fileUriToPath("file:///a%"), FileUriError);
    EXPECT_THROW(fileUriToPath("file:///a%00b"), FileUriError);
}

TEST(FileUri, APathIsWrittenWithUnreservedCharactersLiteral) {
    EXPECT_EQ(pathToFileUri("/out/frame-000003.wav"), "file:///out/frame-000003.wav");
    EXPECT_EQ(pathToFileUri("/a_b~c"), "file:///a_b~c");
}

TEST(FileUri, EverythingElseIsPercentEncoded) {
    EXPECT_EQ(pathToFileUri("/a b/%#?\xE2\x82\xAC"), "file:///a%20b/%25%23%3F%E2%82%AC");
}

TEST(FileUri, ARelativePathIsRefused) {
    EXPECT_THROW(static_cast<void>(pathToFileUri("out/frame.wav")), FileUriError);
}

TEST(FileUri, WritingThenReadingGivesThePathBack) {
    for (const std::string path :
        {"/plain", "/with space", "/odd#?%&=+;,chars", "/\xC3\xA9t\xC3\xA9"}) {
        EXPECT_EQ(fileUriToPath(pathToFileUri(path)), std::filesystem::path{path}) << path;
    }
}

} // namespace
