#pragma once

#include "vitadaw/application/DawApplication.h"
#include "vitadaw/commands/CommandDispatcher.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace vitadaw::platform::juce_adapter {

class TimelineComponent final : public juce::Component,
                                private juce::ScrollBar::Listener {
public:
    TimelineComponent(commands::ICommandDispatcher&,
                      const application::DawApplication&);
    std::function<void(const commands::CommandResult&)> commandCompleted;

    void refreshModel(bool resetViewport = false);
    void setTransportState(const transport::TransportState&);
    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&,
                        const juce::MouseWheelDetails&) override;
    bool keyPressed(const juce::KeyPress&) override;

private:
    struct Hit { const ui::timeline::ClipSnapshot* clip{};
                 ui::timeline::GestureKind kind{ui::timeline::GestureKind::none}; };
    static constexpr int headerWidth = 144;
    static constexpr int toolbarHeight = 34;
    static constexpr int rulerHeight = 28;
    static constexpr int laneHeight = 58;
    static constexpr int scrollBarThickness = 16;

    [[nodiscard]] juce::Rectangle<int> viewportBounds() const noexcept;
    [[nodiscard]] Hit timelineHitTest(juce::Point<float>) const noexcept;
    [[nodiscard]] juce::Rectangle<float> clipBounds(
        std::size_t trackIndex, const ui::timeline::ClipSnapshot&) const noexcept;
    [[nodiscard]] const ui::timeline::ClipSnapshot* selectedClip() const noexcept;
    void updateScrollBars();
    void setZoom(double, double anchorX);
    void dispatch(std::optional<commands::Command>);
    void scrollBarMoved(juce::ScrollBar*, double) override;

    commands::ICommandDispatcher& dispatcher_;
    const application::DawApplication& application_;
    ui::timeline::TimelineSnapshot snapshot_;
    ui::timeline::TimelineInteraction interaction_;
    ui::timeline::CoordinateTransform transform_;
    transport::TransportState transport_;
    double verticalOffset_{};
    juce::ScrollBar horizontal_{false};
    juce::ScrollBar vertical_{true};
    juce::TextButton zoomOut_{"-"}, zoomIn_{"+"};
    juce::TextButton split_{"Split @ Playhead"}, duplicate_{"Duplicate"}, delete_{"Delete"};
    juce::ComboBox rulerMode_;
};

} // namespace vitadaw::platform::juce_adapter
