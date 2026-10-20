#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::AudioProcessorValueTreeState::ParameterLayout BridgeProcessor::makeParameters() {
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"enabled", 1}, "Network enabled", true));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"gain", 1}, "Network gain (dB)",
        juce::NormalisableRange<float>{-60.0f, 12.0f, 0.1f}, 0.0f));
    return layout;
}

BridgeProcessor::BridgeProcessor()
    : AudioProcessor(BusesProperties()
          .withInput("Input", juce::AudioChannelSet::stereo(), true)
          .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, "LAN MIC", makeParameters()) {
    gainParam_ = parameters.getRawParameterValue("gain");
    enabledParam_ = parameters.getRawParameterValue("enabled");
    parameters.addParameterListener("enabled", this);
}

BridgeProcessor::~BridgeProcessor() {
    parameters.removeParameterListener("enabled", this);
    network.setEnabled(false);
}

void BridgeProcessor::parameterChanged(const juce::String&, float value) {
    if constexpr (sending) {
        // Also release the lease when the DAW has stopped calling processBlock().
        if (value < 0.5f) network.setEnabled(false);
    } else {
        // Receiver control-plane activity must not depend on the editor or on
        // the host feeding an input-silent effect into processBlock().
        network.setEnabled(value >= 0.5f && !offline_.load());
    }
}

void BridgeProcessor::resetSendResampler() {
    pendingSamples_ = 0;
    sendInputIndex_ = 0;
    nextSendOutputPosition_ = 0.0;
    previousSendSample_ = 0.0f;
    havePreviousSendSample_ = false;
}

void BridgeProcessor::appendNetworkSample(float sample, uint32_t now) {
    pending_[pendingSamples_++] = sos::pcm16(sample);
    if (pendingSamples_ == pending_.size()) {
        network.enqueue(pending_, now, sequence_++);
        pendingSamples_ = 0;
    }
}

void BridgeProcessor::resampleSendSample(float sample, uint32_t now) {
    if (!havePreviousSendSample_) {
        previousSendSample_ = sample;
        havePreviousSendSample_ = true;
        appendNetworkSample(sample, now);
        nextSendOutputPosition_ = sendStep_;
        return;
    }

    ++sendInputIndex_;
    const double currentPosition = double(sendInputIndex_);
    while (nextSendOutputPosition_ <= currentPosition + 1.0e-9) {
        const auto fraction = float(std::clamp(
            nextSendOutputPosition_ - (currentPosition - 1.0), 0.0, 1.0));
        appendNetworkSample(previousSendSample_ +
            (sample - previousSendSample_) * fraction, now);
        nextSendOutputPosition_ += sendStep_;
    }
    previousSendSample_ = sample;
}

bool BridgeProcessor::isBusesLayoutSupported(const BusesLayout& layout) const {
    const auto out = layout.getMainOutputChannelSet();
    return (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo()) &&
           out == layout.getMainInputChannelSet();
}

void BridgeProcessor::prepareToPlay(double rate, int) {
    dawSampleRate_ = rate > 0.0 ? rate : double(sos::kSampleRate);
    dawSampleRateHz_.store(uint32_t(std::lround(dawSampleRate_)));
    sendStep_ = std::max(0.0001, dawSampleRate_ / double(sos::kSampleRate));
    network.setReceiveSampleRate(dawSampleRate_);
    const bool offline = isNonRealtime();
    offline_.store(offline);
    network.setEnabled(!sending && enabledParam_->load() > 0.5f && !offline);
    network.clearReceive();
    resetSendResampler();
    lastBlockMs_ = 0;
    wasActive_ = false;
    gain_.reset(dawSampleRate_, 0.02);
    gain_.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(gainParam_->load()));
}

void BridgeProcessor::releaseResources() {
    network.setEnabled(false);
    resetSendResampler();
    wasActive_ = false;
}

void BridgeProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    juce::ScopedNoDenormals noDenormals;
    midi.clear();
    const bool offline = isNonRealtime();
    offline_ = offline;
    const bool active = enabledParam_->load() > 0.5f && !offline;
    network.setEnabled(active);
    const auto now = juce::Time::getMillisecondCounter();
    const auto maxGap = std::max(uint32_t(250),
        uint32_t(double(buffer.getNumSamples()) * 3000.0 / dawSampleRate_));
    if (active != wasActive_ || uint32_t(now - lastBlockMs_) > maxGap) {
        resetSendResampler();
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
                resampleSendSample(
                    mono / float(inputChannels) * gain_.getNextValue(), now);
            }
        } else {
            resetSendResampler();
            gain_.skip(buffer.getNumSamples());
        }
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
    resetSendResampler();
    wasActive_ = false;
    midi.clear();
    if constexpr (!sending) buffer.clear();
}

juce::String BridgeProcessor::displayStatus() const {
    if (offline_.load()) return "Offline export: network audio is disabled";
    if (enabledParam_->load() < 0.5f) return "Network disabled";
    auto status = network.status();
    const auto rate = dawSampleRateHz_.load();
    if (rate != sos::kSampleRate) {
        status += sending
            ? " | Resampling " + juce::String(rate) + " -> 48000 Hz"
            : " | Resampling 48000 -> " + juce::String(rate) + " Hz";
    }
    return status;
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
    if constexpr (!sending)
        network.setEnabled(enabledParam_->load() > 0.5f && !offline_.load());
}

juce::AudioProcessorEditor* BridgeProcessor::createEditor() { return new BridgeEditor(*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new BridgeProcessor(); }
