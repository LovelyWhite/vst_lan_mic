#pragma once
#include "PluginProcessor.h"

class BridgeEditor final : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit BridgeEditor(BridgeProcessor&);
    ~BridgeEditor() override { stopTimer(); }
    void paint(juce::Graphics&) override;
    void resized() override;
private:
    void timerCallback() override;
    BridgeProcessor& processor_;
    juce::Label title_, direction_, deviceLabel_, gainLabel_, status_, counters_, hint_;
    juce::ComboBox device_;
    juce::Slider gain_;
    juce::ToggleButton enabled_{"Network enabled"}, monitor_{"Local pass-through"};
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> gainAttachment_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> enabledAttachment_, monitorAttachment_;
    juce::StringArray deviceIds_;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BridgeEditor)
};
