// SPDX-License-Identifier: Apache-2.0

// Tests for the HTTP PlaybackApi's cancel(), against a real HTTP server.
//
// The session's own tests drive a scripted fake; what they cannot show is
// that cancel() really ends a call blocked in cpp-httplib. Here an in-process
// server stalls a render, as a busy or hung engine would, and the call must
// end when it is cancelled rather than at the client's read timeout.

#include "rainbohz/playback_client/playback_api.hpp"

#include <gtest/gtest.h>
#include <httplib.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace rainbohz::playback_client {
namespace {

using namespace std::chrono_literals;

/// A server on a free local port whose render endpoint stalls until
/// released, or for at most ten seconds.
class StallingServer {
public:
    StallingServer() {
        server_.Post("/v1/frames/:frame/renderings", [this](const httplib::Request&,
                                                         httplib::Response& response) {
            std::unique_lock lock{mutex_};
            released_.wait_for(lock, 10s, [this] { return release_; });
            response.set_content(R"({"frame": 0, "uri": "file:///tmp/x.wav"})", "application/json");
        });
        port_ = server_.bind_to_any_port("127.0.0.1");
        thread_ = std::thread{[this] { server_.listen_after_bind(); }};
        server_.wait_until_ready();
    }

    ~StallingServer() {
        {
            const std::scoped_lock lock{mutex_};
            release_ = true;
        }
        released_.notify_all();
        server_.stop();
        thread_.join();
    }

    StallingServer(const StallingServer&) = delete;
    StallingServer& operator=(const StallingServer&) = delete;
    StallingServer(StallingServer&&) = delete;
    StallingServer& operator=(StallingServer&&) = delete;

    /// The API's base URL on this server.
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/v1"; }

private:
    httplib::Server server_;
    int port_ = 0;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable released_;
    bool release_ = false;
};

TEST(HttpPlaybackApi, CancelEndsAStalledCall) {
    const StallingServer server;
    const std::unique_ptr<PlaybackApi> api = makeHttpPlaybackApi(server.url());

    std::thread canceller{[&api] {
        std::this_thread::sleep_for(300ms);
        api->cancel();
    }};
    const auto started = std::chrono::steady_clock::now();
    EXPECT_THROW(api->renderFrame(FrameNumber{0}), PlaybackApiError);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    canceller.join();

    // Ended by the cancel, not by the server's ten seconds or the client's
    // five-minute read timeout.
    EXPECT_GE(elapsed, 250ms);
    EXPECT_LT(elapsed, 3s);
}

TEST(HttpPlaybackApi, ACallAfterCancelFailsAtOnce) {
    const StallingServer server;
    const std::unique_ptr<PlaybackApi> api = makeHttpPlaybackApi(server.url());
    api->cancel();

    const auto started = std::chrono::steady_clock::now();
    std::string message;
    int status = -1;
    try {
        (void)api->renderFrame(FrameNumber{0});
    } catch (const PlaybackApiError& error) {
        message = error.what();
        status = error.status();
    }
    EXPECT_NE(message.find("cancelled"), std::string::npos) << "message: " << message;
    EXPECT_EQ(status, 0);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 1s);
}

TEST(HttpPlaybackApi, CancelWithNoCallInFlightIsHarmless) {
    const std::unique_ptr<PlaybackApi> api = makeHttpPlaybackApi("http://127.0.0.1:1/v1");
    api->cancel();
    api->cancel();
}

} // namespace
} // namespace rainbohz::playback_client
