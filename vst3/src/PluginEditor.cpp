#include "PluginEditor.h"

namespace {
constexpr int kWidth = 660;
constexpr int kMargin = 18;
constexpr int kLabelWidth = 96;
constexpr int kTitleHeight = 30;
constexpr int kDirectionHeight = 20;
constexpr int kHeaderGap = 16;
constexpr int kControlHeight = 30;
constexpr int kRowGap = 10;
constexpr int kToggleHeight = 28;
constexpr int kStatusHeight = 26;
constexpr int kFooterHeight = 18;
constexpr int kFooterBlock = kRowGap + kFooterHeight;
// Single source of truth: resized() consumes these terms in this same order.
constexpr int kBaseHeight = 2 * kMargin + kTitleHeight + kDirectionHeight + kHeaderGap
    + kControlHeight + kRowGap   // device row
    + kControlHeight + kRowGap   // gain row
    + kToggleHeight + kRowGap    // network switch
    + kStatusHeight;             // status pill

const juce::Colour kBackground{0xff141a21};
const juce::Colour kPanel{0xff1e2831};
const juce::Colour kBorder{0xff2d3b46};
const juce::Colour kAccent{0xff45cbb7};
const juce::Colour kText{0xffe9eff3};
const juce::Colour kMuted{0xff8ea2ad};
} // namespace

BridgeEditor::BridgeEditor(BridgeProcessor& processor)
    : AudioProcessorEditor(processor), processor_(processor) {
    setOpaque(true);
    // Colour-only skin, so the device popup and the tooltips match the panel.
    skin_.setColour(juce::PopupMenu::backgroundColourId, kPanel);
    skin_.setColour(juce::PopupMenu::textColourId, kText);
    skin_.setColour(juce::PopupMenu::highlightedBackgroundColourId, kAccent.withAlpha(0.30f));
    skin_.setColour(juce::PopupMenu::highlightedTextColourId, kText);
    skin_.setColour(juce::TooltipWindow::backgroundColourId, kPanel);
    skin_.setColour(juce::TooltipWindow::textColourId, kText);
    skin_.setColour(juce::TooltipWindow::outlineColourId, kBorder);
    setLookAndFeel(&skin_);

    title_.setText(processor.getName(), juce::dontSendNotification);
    title_.setFont(juce::FontOptions(26.0f, juce::Font::bold));
    title_.setColour(juce::Label::textColourId, kText);
    direction_.setText(BridgeProcessor::sending ? "DAW -> net -> Lan mic"
                                                : "Lan mic playback -> net -> DAW",
                       juce::dontSendNotification);
    direction_.setFont(juce::FontOptions(13.0f));
    direction_.setColour(juce::Label::textColourId, kMuted);
    deviceLabel_.setText("Device", juce::dontSendNotification);
    gainLabel_.setText("Network gain", juce::dontSendNotification);
    for (auto* label : {&deviceLabel_, &gainLabel_}) {
        label->setFont(juce::FontOptions(13.0f));
        label->setColour(juce::Label::textColourId, kMuted);
    }
    status_.setFont(juce::FontOptions(13.0f));
    status_.setColour(juce::Label::textColourId, kText);
    status_.setMinimumHorizontalScale(0.65f);

    device_.setColour(juce::ComboBox::backgroundColourId, kPanel);
    device_.setColour(juce::ComboBox::textColourId, kText);
    device_.setColour(juce::ComboBox::outlineColourId, kBorder);
    device_.setColour(juce::ComboBox::buttonColourId, kBorder);
    device_.setColour(juce::ComboBox::arrowColourId, kAccent);
    device_.setTitle("Device");

    gain_.setSliderStyle(juce::Slider::LinearHorizontal);
    gain_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 84, kControlHeight - 6);
    gain_.setTextValueSuffix(" dB");
    gain_.setColour(juce::Slider::trackColourId, kBorder);
    gain_.setColour(juce::Slider::thumbColourId, kAccent);
    gain_.setColour(juce::Slider::textBoxTextColourId, kText);
    gain_.setColour(juce::Slider::textBoxBackgroundColourId, kPanel);
    gain_.setColour(juce::Slider::textBoxOutlineColourId, kBorder);
    gain_.setTitle("Network gain");

    enabled_.setColour(juce::ToggleButton::textColourId, kText);
    enabled_.setColour(juce::ToggleButton::tickColourId, kAccent);
    enabled_.setColour(juce::ToggleButton::tickDisabledColourId, kBorder);

    credit_.setFont(juce::FontOptions(11.0f), false, juce::Justification::centredRight);
    credit_.setColour(juce::HyperlinkButton::textColourId, kMuted);
    credit_.setTooltip("https://lan-mic.suxitech.cn/");

    for (auto* component : {&title_, &direction_, &deviceLabel_,
                            &gainLabel_, &status_})
        addAndMakeVisible(component);
    addAndMakeVisible(device_);
    addAndMakeVisible(gain_);
    addAndMakeVisible(enabled_);
    addAndMakeVisible(credit_);

    gainAttachment_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, "gain", gain_);
    enabledAttachment_ = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        processor.parameters, "enabled", enabled_);
    device_.onChange = [this] {
        const int index = device_.getSelectedId() - 2;
        processor_.network.setPreferredDevice(index >= 0 && index < deviceIds_.size()
                                                 ? deviceIds_[index] : juce::String{});
    };
    setSize(kWidth, kBaseHeight + kFooterBlock);
    timerCallback();
    startTimerHz(4);
}

void BridgeEditor::paint(juce::Graphics& g) {
    const auto bounds = getLocalBounds();
    g.setColour(kBackground);
    g.fillRect(bounds);
    g.setGradientFill(juce::ColourGradient(juce::Colour(0x0dffffff), 0.0f, 0.0f,
                                           juce::Colour(0x00ffffff), 0.0f,
                                           float(bounds.getHeight()), false));
    g.fillRect(bounds);

    g.setColour(kPanel);
    const auto pill = status_.getBounds().expanded(10, 5).toFloat();
    g.fillRoundedRectangle(pill, 8.0f);
    g.setColour(kBorder);
    g.drawRoundedRectangle(pill.reduced(0.5f), 8.0f, 1.0f);

    g.setColour(kAccent);
    g.fillRect(0, 0, bounds.getWidth(), 3);
}

void BridgeEditor::resized() {
    auto area = getLocalBounds().reduced(kMargin);
    auto titleRow = area.removeFromTop(kTitleHeight);
    title_.setBounds(titleRow);
    direction_.setBounds(area.removeFromTop(kDirectionHeight));
    area.removeFromTop(kHeaderGap);
    auto deviceRow = area.removeFromTop(kControlHeight);
    deviceLabel_.setBounds(deviceRow.removeFromLeft(kLabelWidth));
    device_.setBounds(deviceRow);
    area.removeFromTop(kRowGap);
    auto gainRow = area.removeFromTop(kControlHeight);
    gainLabel_.setBounds(gainRow.removeFromLeft(kLabelWidth));
    gain_.setBounds(gainRow);
    area.removeFromTop(kRowGap);
    auto switchRow = area.removeFromTop(kToggleHeight);
    enabled_.setBounds(switchRow.removeFromLeft(210));
    area.removeFromTop(kRowGap);
    status_.setBounds(area.removeFromTop(kStatusHeight));
    area.removeFromTop(kRowGap);
    // The hit area must match the right-aligned text: a full-width row turned
    // the empty space left of the credit into accidental clicks.
    const auto creditWidth = juce::GlyphArrangement::getStringWidthInt(
        juce::Font(juce::FontOptions(11.0f)), credit_.getButtonText()) + 8;
    credit_.setBounds(area.removeFromTop(kFooterHeight).removeFromRight(creditWidth));
}

void BridgeEditor::timerCallback() {
    const auto selected = processor_.network.preferredDevice();
    juce::StringArray ids;
    // Only discovered devices are listed; an unreachable preference must not
    // linger as a ghost entry.
    for (const auto& device : processor_.network.devices()) ids.addIfNotAlreadyThere(device.identity);
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
}
