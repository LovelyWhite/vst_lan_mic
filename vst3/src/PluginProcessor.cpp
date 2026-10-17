#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::AudioProcessorValueTreeState::ParameterLayout BridgeProcessor::makeParameters() {
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"enabled", 1}, "Network enabled", true));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"gain", 1}, "Network gain (dB)",
        juce::NormalisableRange<float>{-60.0f, 12.0f, 0.1f}, 0.0f));
    if constexpr (sending)
        layout.add(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"monitor", 1}, "Local pass-through", true));
    return layout;
}

BridgeProcessor::BridgeProcessor()
    : AudioProcessor(BusesProperties()
          .withInput("Input", juce::AudioChannelSet::stereo(), true)
          .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, "StudioOneStream", makeParameters()) {
    gainParam_ = parameters.getRawParameterValue("gain");
    enabledParam_ = parameters.getRawParameterValue("enabled");
    if constexpr (sending) monitorParam_ = parameters.getRawParameterValue("monitor");
    parameters.addParameterListener("enabled", this);
}

BridgeProcessor::~BridgeProcessor() {
    parameters.removeParameterListener("enabled", this);
    network.setEnabled(false);
}

void BridgeProcessor::parameterChanged(const juce::String&, float value) {
    // Also release the lease when the DAW has stopped calling processBlock().
    if (value < 0.5f) network.setEnabled(false);
}

bool BridgeProcessor::isBusesLayoutSupported(const BusesLayout& layout) const {
    const auto out = layout.getMainOutputChannelSet();
    return (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo()) &&
           out == layout.getMainInputChannelSet();
}

void BridgeProcessor::prepareToPlay(double rate, int) {
    rateOK_ = std::abs(rate - double(sos::kSampleRate)) < 0.01;
    network.setEnabled(false);
    network.clearReceive();
    pendingSamples_ = 0;
    lastBlockMs_ = 0;
    wasActive_ = false;
    gain_.reset(rate > 0 ? rate : sos::kSampleRate, 0.02);
    gain_.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(gainParam_->load()));
}

void BridgeProcessor::releaseResources() {
    network.setEnabled(false);
    rateOK_ = false;
    pendingSamples_ = 0;
    wasActive_ = false;
}

void BridgeProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    juce::ScopedNoDenormals noDenormals;
    midi.clear();
    const bool offline = isNonRealtime();
    offline_ = offline;
    const bool active = enabledParam_->load() > 0.5f && rateOK_.load() && !offline;
    network.setEnabled(active);
    const auto now = juce::Time::getMillisecondCounter();
    const auto maxGap = std::max(uint32_t(250),
        uint32_t(double(buffer.getNumSamples()) * 3000.0 / sos::kSampleRate));
    if (active != wasActive_ || uint32_t(now - lastBlockMs_) > maxGap) {
        pendingSamples_ = 0;
        network.clearReceive();
        wasActive_ = active;
    }
    lastBlockMs_ = now;
    gain_.setTargetValue(juce::Decibels::decibelsToGain(gainParam_->load()));
    const int channels = buffer.getNumChannels();
    if (channels == 0) return;

    if constexpr (sending) {
        const int inputChannels = std::min(getTotalNumInputChannels(), channels);
        if (active && network.connected() && inputChannels > 0) {
            for (int i = 0; i < buffer.getNumSamples(); ++i) {
                float mono = 0;
                for (int ch = 0; ch < inputChannels; ++ch)
                    mono += buffer.getSample(ch, i);
                pending_[pendingSamples_++] = sos::pcm16(mono / float(inputChannels) * gain_.getNextValue());
                if (pendingSamples_ == pending_.size()) {
                    network.enqueue(pending_, juce::Time::getMillisecondCounter(), sequence_++);
                    pendingSamples_ = 0;
                }
            }
        } else {
            pendingSamples_ = 0;
            gain_.skip(buffer.getNumSamples());
        }
        if (monitorParam_->load() < 0.5f) buffer.clear();
    } else {
        network.prepareReceive();
        if (!active || !network.connected()) {
            network.clearReceive();
            buffer.clear();
            gain_.skip(buffer.getNumSamples());
            return;
        }
        for (int i = 0; i < buffer.getNumSamples(); ++i) {
            const auto mono = std::clamp(network.receiveSample() * gain_.getNextValue(), -1.0f, 1.0f);
            for (int ch = 0; ch < channels; ++ch) buffer.setSample(ch, i, mono);
        }
    }
}

void BridgeProcessor::processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    network.setEnabled(false);
    network.clearReceive();
    pendingSamples_ = 0;
    wasActive_ = false;
    midi.clear();
    if constexpr (!sending) buffer.clear();
}

juce::String BridgeProcessor::displayStatus() const {
    if (!rateOK_.load()) return "Set the DAW sample rate to 48000 Hz";
    if (offline_.load()) return "Offline export: network audio is disabled";
    if (enabledParam_->load() < 0.5f) return "Network disabled";
    return network.status();
}

void BridgeProcessor::getStateInformation(juce::MemoryBlock& destination) {
    auto state = parameters.copyState();
    state.setProperty("device", network.preferredDevice(), nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary(*xml, destination);
}

void BridgeProcessor::setStateInformation(const void* data, int size) {
    const auto xml = getXmlFromBinary(data, size);
    if (!xml || !xml->hasTagName(parameters.state.getType())) return;
    auto state = juce::ValueTree::fromXml(*xml);
    if (!state.isValid()) return;
    network.setPreferredDevice(state.getProperty("device").toString());
    parameters.replaceState(state);
}

juce::AudioProcessorEditor* BridgeProcessor::createEditor() { return new BridgeEditor(*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new BridgeProcessor(); }
