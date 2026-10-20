#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <limits>
#include "Transport.h"

class BridgeProcessor final : public juce::AudioProcessor,
                              private juce::AudioProcessorValueTreeState::Listener {
public:
    static constexpr bool sending = SOS_SENDING != 0;
    BridgeProcessor();
    ~BridgeProcessor() override;
    void prepareToPlay(double sampleRate, int maximumBlockSize) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override {
        return sending ? 0.0 : std::numeric_limits<double>::infinity();
    }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;
    juce::String displayStatus() const;

    juce::AudioProcessorValueTreeState parameters;
    Transport network{sending};
private:
    static juce::AudioProcessorValueTreeState::ParameterLayout makeParameters();
    void parameterChanged(const juce::String&, float value) override;
    void resetSendResampler();
    void appendNetworkSample(float sample, uint32_t now);
    void resampleSendSample(float sample, uint32_t now);
    std::atomic<float>* gainParam_ = nullptr;
    std::atomic<float>* enabledParam_ = nullptr;
    std::atomic<bool> offline_{false};
    std::atomic<uint32_t> dawSampleRateHz_{sos::kSampleRate};
    juce::SmoothedValue<float> gain_;
    sos::Frame pending_{};
    size_t pendingSamples_ = 0;
    uint32_t sequence_ = 0, lastBlockMs_ = 0;
    uint64_t sendInputIndex_ = 0;
    double dawSampleRate_ = sos::kSampleRate;
    double sendStep_ = 1.0, nextSendOutputPosition_ = 0.0;
    float previousSendSample_ = 0.0f;
    bool havePreviousSendSample_ = false;
    bool wasActive_ = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BridgeProcessor)
};
