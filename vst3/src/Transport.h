#pragma once
#include <juce_core/juce_core.h>
#include "AudioBuffer.h"

class Transport final : private juce::Thread {
public:
    enum class State { Searching, Connecting, Connected, Busy, SocketError };
    struct Device { juce::String identity, ip; uint32_t seenMs = 0; };
    explicit Transport(bool sending);
    ~Transport() override;

    void setEnabled(bool value) { enabled_.store(value, std::memory_order_release); }
    bool connected() const { return state_.load() == State::Connected; }
    void setPreferredDevice(const juce::String&);
    juce::String preferredDevice() const;
    juce::String status() const;
    juce::String receiveDiagnostics() const;
    // Zeroes the return-path counters and restarts the ESP delta window.
    void resetDiagnostics();
    std::vector<Device> devices() const;
    void enqueue(const sos::OpusFrame&, uint32_t now);
    void setReceiveSampleRate(double outputRate) {
        const auto safeRate = outputRate > 0.0 ? outputRate : double(sos::kSampleRate);
        incoming_.setNominalRatio(float(double(sos::kSampleRate) / safeRate));
        incoming_.reset();
    }
    void prepareReceive();
    // Writes the next interleaved pair: out[0] = left, out[1] = right.
    void receiveSamples(float* out) { incoming_.next(out); }
    void clearReceive() { incoming_.reset(); }

private:
    struct Outgoing {
        sos::OpusFrame pcm{}; // Interleaved L/R, one 20 ms frame.
        uint32_t timeMs = 0;
    };
    void run() override;
    void invalidateAudio();
    const bool sending_;
    const uint32_t session_;
    // Send-time sequence assignment: only run() touches this, so the
    // receiver only ever sees gaps that really went on the wire.
    uint32_t sendSequence_ = 0;
    std::atomic<bool> enabled_{false};
    std::atomic<State> state_{State::Searching};
    std::atomic<uint32_t> discoveryInterfaces_{0};
    std::atomic<uint32_t> preferenceVersion_{0}, audioEpoch_{0};
    uint32_t consumedEpoch_ = 0; // Audio-thread owned.
    sos::SpscRing<Outgoing, 64> outgoing_;
    sos::StereoPlayout incoming_; // Downlink arrives as interleaved stereo.
    std::atomic<uint32_t> receivePackets_{0}, receivePlc_{0}, receiveLate_{0};
    std::atomic<uint32_t> receiveDecodeErrors_{0}, receiveBufferedMs_{0};
    mutable juce::CriticalSection settingsLock_;
    sos::DeviceDiagnostics deviceDiagnostics_{}, previousDeviceDiagnostics_{};
    bool haveDeviceDiagnostics_ = false;
    uint32_t deviceDiagnosticsAtMs_ = 0, deviceDiagnosticsTimestampMs_ = 0;
    uint32_t deviceDiagnosticsSequence_ = 0, deviceDiagnosticsWindowMs_ = 0;
    juce::String preferred_, selected_;
    std::vector<Device> devices_;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Transport)
};
