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
    std::vector<Device> devices() const;
    void enqueue(const sos::Frame&, uint32_t now, uint32_t sequence);
    void setReceiveSampleRate(double outputRate) {
        const auto safeRate = outputRate > 0.0 ? outputRate : double(sos::kSampleRate);
        incoming_.setNominalRatio(float(double(sos::kSampleRate) / safeRate));
        incoming_.reset();
    }
    void prepareReceive();
    // Writes the next interleaved pair: out[0] = left, out[1] = right.
    void receiveSamples(float* out) { incoming_.next(out); }
    void clearReceive() { incoming_.reset(); }

    std::atomic<uint32_t> sent{0}, received{0}, lost{0}, dropped{0}, socketErrors{0};
    uint32_t underruns() const { return incoming_.underruns.load(); }
    uint32_t trims() const { return incoming_.trims.load(); }

private:
    struct Outgoing {
        sos::Frame pcm{};
        uint32_t timeMs = 0, sequence = 0;
    };
    void run() override;
    void invalidateAudio();
    const bool sending_;
    const uint32_t session_;
    std::atomic<bool> enabled_{false};
    std::atomic<State> state_{State::Searching};
    std::atomic<uint32_t> discoveryInterfaces_{0};
    std::atomic<uint32_t> preferenceVersion_{0}, audioEpoch_{0};
    uint32_t consumedEpoch_ = 0; // Audio-thread owned.
    sos::SpscRing<Outgoing, 32> outgoing_;
    sos::StereoPlayout incoming_; // Downlink arrives as interleaved stereo.
    mutable juce::CriticalSection settingsLock_;
    juce::String preferred_, selected_;
    std::vector<Device> devices_;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Transport)
};
