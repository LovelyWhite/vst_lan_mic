#pragma once
#include "PluginProcessor.h"
#include "Version.h"

class BridgeEditor final : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit BridgeEditor(BridgeProcessor&);
    ~BridgeEditor() override { stopTimer(); setLookAndFeel(nullptr); }
    void paint(juce::Graphics&) override;
    void resized() override;
private:
    void timerCallback() override;
    // Declared first: the popup menu and tooltips keep pointing at it while the
    // child components are being torn down.
    juce::LookAndFeel_V4 skin_;
    BridgeProcessor& processor_;
    juce::Label title_, direction_, deviceLabel_, gainLabel_, status_;
    juce::ComboBox device_;
    juce::Slider gain_;
    juce::ToggleButton enabled_{"Network enabled"};
    juce::HyperlinkButton credit_{"@suxitech v" LANMIC_VERSION,
                                  juce::URL{"https://lan-mic.suxitech.cn/"}};
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> gainAttachment_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> enabledAttachment_;
    juce::StringArray deviceIds_;
    juce::StringArray deviceNames_;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BridgeEditor)
};
