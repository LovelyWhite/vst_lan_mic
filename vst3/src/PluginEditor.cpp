#include "PluginEditor.h"

BridgeEditor::BridgeEditor(BridgeProcessor& processor)
    : AudioProcessorEditor(processor), processor_(processor) {
    title_.setText(processor.getName(), juce::dontSendNotification);
    title_.setFont(juce::FontOptions(28.0f, juce::Font::bold));
    direction_.setText(BridgeProcessor::sending ? "DAW -> ESP32 -> phone microphone"
                                                : "Phone playback -> ESP32 -> DAW",
                       juce::dontSendNotification);
    deviceLabel_.setText("Device", juce::dontSendNotification);
    gainLabel_.setText("Network gain", juce::dontSendNotification);
    hint_.setText("Auto sample-rate conversion | 48 kHz network stereo | Stereo USB",
                  juce::dontSendNotification);
    hint_.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    status_.setMinimumHorizontalScale(0.65f);
    gain_.setSliderStyle(juce::Slider::LinearHorizontal);
    gain_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 90, 24);
    gain_.setTextValueSuffix(" dB");
    for (auto* component : {&title_, &direction_, &deviceLabel_,
                            &gainLabel_, &status_, &counters_, &hint_})
        addAndMakeVisible(component);
    addAndMakeVisible(device_);
    addAndMakeVisible(gain_);
    addAndMakeVisible(enabled_);

    gainAttachment_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, "gain", gain_);
    enabledAttachment_ = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        processor.parameters, "enabled", enabled_);
    device_.onChange = [this] {
        const int index = device_.getSelectedId() - 2;
        processor_.network.setPreferredDevice(index >= 0 && index < deviceIds_.size()
                                                 ? deviceIds_[index] : juce::String{});
    };
    setSize(620, 310);
    timerCallback();
    startTimerHz(4);
}

void BridgeEditor::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(0xff192129));
    g.setColour(juce::Colour(0xff45cbb7));
    g.fillRect(0, 0, getWidth(), 3);
}

void BridgeEditor::resized() {
    auto area = getLocalBounds().reduced(20);
    title_.setBounds(area.removeFromTop(36));
    direction_.setBounds(area.removeFromTop(26));
    area.removeFromTop(12);
    auto deviceRow = area.removeFromTop(30);
    deviceLabel_.setBounds(deviceRow.removeFromLeft(100));
    device_.setBounds(deviceRow);
    area.removeFromTop(8);
    auto gainRow = area.removeFromTop(32);
    gainLabel_.setBounds(gainRow.removeFromLeft(100));
    gain_.setBounds(gainRow);
    auto switches = area.removeFromTop(36);
    enabled_.setBounds(switches.removeFromLeft(210));
    status_.setBounds(area.removeFromTop(28));
    counters_.setBounds(area.removeFromTop(25));
    hint_.setBounds(area.removeFromTop(24));
}

void BridgeEditor::timerCallback() {
    const auto selected = processor_.network.preferredDevice();
    juce::StringArray ids;
    for (const auto& device : processor_.network.devices()) ids.addIfNotAlreadyThere(device.identity);
    if (selected.isNotEmpty()) ids.addIfNotAlreadyThere(selected);
    ids.sort(true);
    if (ids != deviceIds_ || device_.getNumItems() == 0) {
        deviceIds_ = ids;
        device_.clear(juce::dontSendNotification);
        device_.addItem("Automatic (first discovered device)", 1);
        for (int i = 0; i < deviceIds_.size(); ++i) device_.addItem(deviceIds_[i], i + 2);
    }
    device_.setSelectedId(selected.isEmpty() ? 1 : deviceIds_.indexOf(selected) + 2,
                          juce::dontSendNotification);
    status_.setText(processor_.displayStatus(), juce::dontSendNotification);
    const auto& net = processor_.network;
    counters_.setText("TX " + juce::String(net.sent.load()) +
        "   RX " + juce::String(net.received.load()) +
        "   Lost " + juce::String(net.lost.load()) +
        "   Drop " + juce::String(net.dropped.load()) +
        "   Under " + juce::String(net.underruns()) +
        "   Trim " + juce::String(net.trims()) +
        "   IO " + juce::String(net.socketErrors.load()), juce::dontSendNotification);
}
