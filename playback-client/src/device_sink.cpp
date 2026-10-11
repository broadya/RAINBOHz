// SPDX-License-Identifier: Apache-2.0

#include "rainbohz/playback_client/audio_sink.hpp"

#include "rainbohz/playback_client/log.hpp"
#include "rainbohz/playback_client/ring_buffer.hpp"

#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

/// @file
/// @brief The default output device, through miniaudio.
///
/// This is the only translation unit that includes miniaudio.h, and the one
/// that carries its implementation, with the decoding, encoding and engine
/// parts compiled out: only the device API is used.
///
/// The feeder writes into a SampleRing; the device's callback, on miniaudio's
/// own real-time thread, pops from it. The callback takes no lock, does no
/// I/O, does not allocate and does not log: when the ring runs dry it plays
/// silence and counts the samples it had to make up, which the session
/// reports afterwards. Waiting is on the feeder's side only, where write()
/// polls for room. See docs/design.md §5.
///
/// @see audio_sink.hpp

namespace rainbohz::playback_client {

namespace {

/// How long write() and finish() sleep between looks at the ring.
constexpr std::chrono::milliseconds kPollInterval{2};

/// The ring holds this fraction of a second of audio.
constexpr int kRingDivisor = 2;

/// The largest number of samples the callback pops at once; a larger
/// request is served in chunks of this size.
constexpr std::size_t kScratchSamples = 4096;

/// A miniaudio device, uninitialised when it is destroyed if it was ever
/// initialised.
///
/// Owning the device in a member of its own, rather than in DeviceSink's
/// destructor, means it is closed even when DeviceSink's constructor throws
/// after ma_device_init() has succeeded: a destructor does not run for an
/// object whose constructor did not finish, but its finished members are
/// still destroyed. Neither copyable nor movable, because miniaudio keeps
/// pointers into the struct.
struct OwnedDevice {
    /// A C struct that ma_device_init() fills in; zeroing it first is what
    /// miniaudio's own examples do, and its enum member is set there.
    ma_device device{}; // NOLINT(bugprone-invalid-enum-default-initialization)
    /// Whether ma_device_init() succeeded, so the destructor must uninit it.
    bool initialised = false;

    OwnedDevice() = default;

    /// Closes the device, stopping it and its callback first if it runs.
    ~OwnedDevice() {
        if (initialised) {
            ma_device_uninit(&device);
        }
    }

    OwnedDevice(const OwnedDevice&) = delete;
    OwnedDevice& operator=(const OwnedDevice&) = delete;
    OwnedDevice(OwnedDevice&&) = delete;
    OwnedDevice& operator=(OwnedDevice&&) = delete;
};

/// The default output device, fed from a ring buffer.
class DeviceSink final : public AudioSink {
public:
    /// A sink for samples at `rate`, with its device opened but not started.
    ///
    /// @param rate  The rate of the samples written.
    /// @throws std::runtime_error if the device cannot be opened.
    explicit DeviceSink(SampleRate rate)
        : ring_{std::max<std::size_t>(
              static_cast<std::size_t>(rate.value() / kRingDivisor), kScratchSamples)},
          scratch_(kScratchSamples) {
        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_f32;
        config.playback.channels = 0; // the device's own channel count
        config.sampleRate = static_cast<ma_uint32>(rate.value());
        config.dataCallback = &DeviceSink::callback;
        config.notificationCallback = &DeviceSink::notification;
        config.pUserData = this;
        if (ma_device_init(nullptr, &config, &device_.device) != MA_SUCCESS) {
            throw std::runtime_error{"cannot open the default audio output device"};
        }
        device_.initialised = true;

        const ma_device& device = device_.device;
        const ma_uint32 deviceRate = device.playback.internalSampleRate;
        logLine(LogLevel::Info, "audio device: " + std::string{std::data(device.playback.name)} +
                                    ", " + std::to_string(device.playback.channels) +
                                    " channel(s) at " + std::to_string(deviceRate) + " Hz");
        if (std::cmp_not_equal(deviceRate, rate.value())) {
            logLine(LogLevel::Info, "the device runs at " + std::to_string(deviceRate) +
                                        " Hz; resampling from " + std::to_string(rate.value()) +
                                        " Hz");
        }
    }

    /// Closes the device, through device_, the first member destroyed.
    ~DeviceSink() override = default;

    DeviceSink(const DeviceSink&) = delete;
    DeviceSink& operator=(const DeviceSink&) = delete;
    DeviceSink(DeviceSink&&) = delete;
    DeviceSink& operator=(DeviceSink&&) = delete;

    void start() override {
        if (ma_device_start(&device_.device) != MA_SUCCESS) {
            throw std::runtime_error{"cannot start the default audio output device"};
        }
    }

    bool write(std::span<const float> samples) override {
        primed_.store(true, std::memory_order_release);
        while (!samples.empty()) {
            if (aborted_.load()) {
                return false;
            }
            throwIfDeviceLost();
            const std::size_t pushed = ring_.push(samples);
            samples = samples.subspan(pushed);
            if (!samples.empty()) {
                std::this_thread::sleep_for(kPollInterval);
            }
        }
        return true;
    }

    void finish() override {
        finishing_.store(true, std::memory_order_release);
        while (ring_.size() > 0 && !aborted_.load()) {
            throwIfDeviceLost();
            std::this_thread::sleep_for(kPollInterval);
        }
        // The ring is empty, but the device still holds what it took last:
        // up to its whole buffer, periods × period size. Let that play out.
        const double bufferedSeconds =
            static_cast<double>(device_.device.playback.internalPeriodSizeInFrames *
                                device_.device.playback.internalPeriods) /
            static_cast<double>(std::max<ma_uint32>(device_.device.playback.internalSampleRate, 1));
        std::this_thread::sleep_for(
            std::chrono::duration<double>(bufferedSeconds) + std::chrono::milliseconds{50});
        ma_device_stop(&device_.device);
    }

    void abort() noexcept override {
        aborted_.store(true);
        finishing_.store(true);
    }

    SampleCount underrun() const noexcept override {
        return SampleCount{static_cast<std::int64_t>(underrun_.load())};
    }

private:
    /// Fails when the device stopped without being asked to.
    ///
    /// @throws std::runtime_error if notification() saw the device stop
    ///         before finish(): the callback no longer drains the ring, so
    ///         waiting for room or for the ring to empty would never end.
    void throwIfDeviceLost() const {
        if (deviceLost_.load(std::memory_order_acquire)) {
            throw std::runtime_error{"the audio output device stopped (disconnected, or taken "
                                     "by another application?)"};
        }
    }

    /// miniaudio's notification callback: records a device that stopped by
    /// itself.
    ///
    /// The device stops when finish() stops it, when it is closed, and when
    /// the backend loses it -- unplugged, say. Only the last is a failure:
    /// the first two happen after finish() or abort(), which set finishing_.
    /// Runs on a miniaudio thread, so it only sets an atomic.
    ///
    /// @param notification  What happened, and to which device.
    static void notification(const ma_device_notification* notification) {
        if (notification->type != ma_device_notification_type_stopped) {
            return;
        }
        auto& self = *static_cast<DeviceSink*>(notification->pDevice->pUserData);
        if (!self.finishing_.load(std::memory_order_acquire)) {
            self.deviceLost_.store(true, std::memory_order_release);
        }
    }

    /// miniaudio's data callback: fills the device's buffer from the ring.
    ///
    /// Runs on miniaudio's real-time thread. Real-time safe: it pops from a
    /// lock-free ring into preallocated scratch, writes zeros where the ring
    /// ran dry, and touches only atomics besides.
    ///
    /// @param device       The device; its user data is the DeviceSink.
    /// @param[out] output  The device's interleaved float buffer, `frames` ×
    ///                     channels samples; every sample is written.
    /// @param input        Unused; a playback device has no input.
    /// @param frames       Sample frames the device wants.
    static void callback(ma_device* device, void* output, const void* input, ma_uint32 frames) {
        (void)input;
        auto& self = *static_cast<DeviceSink*>(device->pUserData);
        const ma_uint32 channels = device->playback.channels;
        const std::span<float> out{
            static_cast<float*>(output), static_cast<std::size_t>(frames) * channels};

        std::size_t done = 0;
        while (done < frames) {
            const std::size_t want = std::min<std::size_t>(frames - done, self.scratch_.size());
            const std::size_t got =
                self.aborted_.load(std::memory_order_relaxed)
                    ? 0
                    : self.ring_.pop(std::span<float>{self.scratch_.data(), want});
            for (std::size_t i = 0; i < want; ++i) {
                const float sample = i < got ? self.scratch_[i] : 0.0F;
                for (ma_uint32 channel = 0; channel < channels; ++channel) {
                    out[((done + i) * channels) + channel] = sample;
                }
            }
            done += want;
            if (got < want) {
                // The ring is dry; the rest of this buffer is silence. It is
                // an underrun if audio was expected.
                const std::size_t silent = (want - got) + (frames - done);
                if (self.primed_.load(std::memory_order_acquire) &&
                    !self.finishing_.load(std::memory_order_acquire)) {
                    self.underrun_.fetch_add(silent, std::memory_order_relaxed);
                }
                std::ranges::fill(out.subspan(done * channels), 0.0F);
                break;
            }
        }
    }

    /// Samples written and not yet taken by the callback.
    SampleRing ring_;
    /// The callback's working buffer, allocated once here.
    std::vector<float> scratch_;
    /// Set by the first write(): from then on a dry ring is an underrun.
    std::atomic<bool> primed_{false};
    /// Set by finish() or abort(): from then on a dry ring is expected.
    std::atomic<bool> finishing_{false};
    /// Set by abort(): write() returns false and the callback plays silence.
    std::atomic<bool> aborted_{false};
    /// Silent samples the callback played while audio was expected (underrun()).
    std::atomic<std::uint64_t> underrun_{0};
    /// Set by notification() when the device stopped by itself: write() and
    /// finish() then fail rather than wait for a callback that will not come.
    std::atomic<bool> deviceLost_{false};
    /// The miniaudio playback device the callback fills. Declared last, so
    /// that it is destroyed first: closing it stops the callback before
    /// anything the callback touches is destroyed.
    OwnedDevice device_;
};

} // namespace

std::unique_ptr<AudioSink> makeDeviceSink(SampleRate rate) {
    return std::make_unique<DeviceSink>(rate);
}

} // namespace rainbohz::playback_client
