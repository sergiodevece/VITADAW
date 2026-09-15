#include "vitadaw/platform/juce/TimelineComponent.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace vitadaw::platform::juce_adapter {
using ui::timeline::GestureKind;

TimelineComponent::TimelineComponent(commands::ICommandDispatcher& dispatcher,
                                     const application::DawApplication& application)
    : dispatcher_(dispatcher), application_(application),
      snapshot_(application.timelineSnapshot()),
      transform_(snapshot_.projectSampleRate) {
    transport_.synchronise(snapshot_.playback,
                           snapshot_.transportPosition,
                           snapshot_.contentDuration);
    setWantsKeyboardFocus(true);
    setMouseClickGrabsKeyboardFocus(true);
    for (auto* bar : {&horizontal_, &vertical_}) {
        addAndMakeVisible(*bar);
        bar->addListener(this);
    }
    zoomOut_.onClick = [this] { setZoom(transform_.pixelsPerSecond() / 1.5,
                                       viewportBounds().getWidth() * 0.5); };
    zoomIn_.onClick = [this] { setZoom(transform_.pixelsPerSecond() * 1.5,
                                      viewportBounds().getWidth() * 0.5); };
    split_.onClick = [this] { dispatch(interaction_.splitCommand(snapshot_, transport_.position)); };
    duplicate_.onClick = [this] { dispatch(interaction_.duplicateCommand(snapshot_)); };
    delete_.onClick = [this] { dispatch(interaction_.deleteCommand()); };
    for (auto* button : {&zoomOut_, &zoomIn_, &split_, &duplicate_, &delete_})
        addAndMakeVisible(*button);
    rulerMode_.addItem("Seconds", 1);
    rulerMode_.addItem("Frames", 2);
    rulerMode_.addItem("Bars / Beats", 3);
    rulerMode_.setSelectedId(1);
    rulerMode_.onChange = [this] { repaint(); }; // presentation only; never dirty
    addAndMakeVisible(rulerMode_);
    updateScrollBars();
}

void TimelineComponent::refreshModel(bool resetViewport) {
    if (!resetViewport && snapshot_.revision == application_.timelineRevision()) return;
    snapshot_ = application_.timelineSnapshot();
    interaction_.reconcile(snapshot_);
    transform_ = ui::timeline::CoordinateTransform{snapshot_.projectSampleRate,
        transform_.pixelsPerSecond(), resetViewport ? 0.0 : transform_.visibleStartSeconds()};
    if (resetViewport) verticalOffset_ = 0.0;
    updateScrollBars();
    repaint();
}

void TimelineComponent::setTransportState(const transport::TransportState& state) {
    transport_ = state;
    const auto editable = state.playback != transport::PlaybackState::playing;
    split_.setEnabled(editable);
    duplicate_.setEnabled(editable);
    delete_.setEnabled(editable);
    refreshModel(false);
    repaint();
}

juce::Rectangle<int> TimelineComponent::viewportBounds() const noexcept {
    return {headerWidth, toolbarHeight + rulerHeight,
            std::max(0, getWidth() - headerWidth - scrollBarThickness),
            std::max(0, getHeight() - toolbarHeight - rulerHeight - scrollBarThickness)};
}

juce::Rectangle<float> TimelineComponent::clipBounds(
    std::size_t trackIndex, const ui::timeline::ClipSnapshot& clip) const noexcept {
    auto start = clip.projectStart;
    auto duration = clip.duration;
    if (interaction_.preview() && interaction_.preview()->id == clip.id) {
        start = interaction_.preview()->projectStart;
        duration = interaction_.preview()->duration;
    }
    const auto left = static_cast<float>(headerWidth + transform_.projectFrameToX(start));
    const auto right = static_cast<float>(headerWidth + transform_.preciseProjectFrameToX(
        static_cast<double>(start.value) + duration.value));
    const auto top = static_cast<float>(toolbarHeight + rulerHeight +
        static_cast<double>(trackIndex * laneHeight) - verticalOffset_ + 7.0);
    return {left, top, std::max(1.0F, right - left), static_cast<float>(laneHeight - 14)};
}

TimelineComponent::Hit TimelineComponent::timelineHitTest(juce::Point<float> point) const noexcept {
    const auto viewport = viewportBounds().toFloat();
    if (!viewport.contains(point)) return {};
    const auto logicalY = point.y - static_cast<float>(toolbarHeight + rulerHeight) +
                          static_cast<float>(verticalOffset_);
    if (logicalY < 0.0F) return {};
    const auto track = static_cast<std::size_t>(logicalY / laneHeight);
    if (track >= snapshot_.tracks.size()) return {};
    // Reverse order makes the later deterministic draw win if rectangles overlap.
    for (auto i = snapshot_.tracks[track].clips.rbegin();
         i != snapshot_.tracks[track].clips.rend(); ++i) {
        const auto bounds = clipBounds(track, *i);
        if (!bounds.intersects(viewport) || !bounds.contains(point)) continue;
        constexpr float handle = 7.0F;
        if (point.x <= bounds.getX() + handle) return {&*i, GestureKind::trimLeft};
        if (point.x >= bounds.getRight() - handle) return {&*i, GestureKind::trimRight};
        return {&*i, GestureKind::move};
    }
    return {};
}

const ui::timeline::ClipSnapshot* TimelineComponent::selectedClip() const noexcept {
    if (!interaction_.selection()) return nullptr;
    for (const auto& lane : snapshot_.tracks)
        for (const auto& clip : lane.clips)
            if (clip.id == *interaction_.selection()) return &clip;
    return nullptr;
}

void TimelineComponent::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour{0xff17191d});
    g.setColour(juce::Colour{0xff252931});
    g.fillRect(0, 0, getWidth(), toolbarHeight);
    g.setColour(juce::Colours::white.withAlpha(0.85F));
    g.setFont(juce::FontOptions{15.0F}.withStyle("Bold"));
    g.drawText("Timeline", 10, 0, 130, toolbarHeight, juce::Justification::centredLeft);

    const auto viewport = viewportBounds();
    g.saveState();
    g.reduceClipRegion({0, toolbarHeight, getWidth() - scrollBarThickness,
                        getHeight() - toolbarHeight - scrollBarThickness});
    g.setColour(juce::Colour{0xff20242a});
    g.fillRect(headerWidth, toolbarHeight, viewport.getWidth(), rulerHeight);
    g.setColour(juce::Colour{0xff292d35});
    g.fillRect(0, toolbarHeight, headerWidth, rulerHeight);

    const auto pixels = transform_.pixelsPerSecond();
    constexpr std::array<double, 12> intervals{0.1, 0.2, 0.5, 1.0, 2.0, 5.0,
                                               10.0, 20.0, 30.0, 60.0, 120.0, 300.0};
    auto tick = intervals.back();
    for (const auto candidate : intervals)
        if (candidate * pixels >= 72.0) { tick = candidate; break; }
    const auto first = std::floor(transform_.visibleStartSeconds() / tick) * tick;
    const auto visibleEnd = transform_.visibleStartSeconds() + viewport.getWidth() / pixels;
    g.setFont(juce::FontOptions{11.0F});
    if (rulerMode_.getSelectedId() == 3) {
        std::array<musical::GridLine, 512> lines;
        const auto& map = application_.musicalTime(); // UI-thread query; no copied events
        const auto rate = snapshot_.projectSampleRate.hertz();
        const auto frame = timeline::ProjectFramePosition{static_cast<std::int64_t>(transform_.visibleStartSeconds() * rate)};
        const auto bpm = map.tempoAt(frame);
        const auto signature = map.timeSignatureAt(frame);
        const auto beatPixels = bpm && signature ? pixels * 60.0 / bpm.value.value * 4 / signature.value.denominator : 0;
        const musical::GridSubdivision density = beatPixels < 32 ? musical::GridSubdivision{musical::GridKind::bars, 1} :
            beatPixels < 160 ? musical::GridSubdivision{musical::GridKind::beats, 1} :
                              musical::GridSubdivision{musical::GridKind::subdivisions, 4};
        const auto result = map.enumerateGridLines({transform_.visibleStartSeconds() * rate}, {visibleEnd * rate}, density, lines);
        float lastLabel = -1000;
        for (std::size_t n = 0; n < result.count; ++n) {
            const auto& line = lines[n];
            const auto x = static_cast<float>(headerWidth + transform_.preciseProjectFrameToX(line.frame.value));
            g.setColour(juce::Colours::white.withAlpha(line.barStart ? 0.35F : 0.12F));
            g.drawVerticalLine(static_cast<int>(std::round(x)), toolbarHeight + 17.0F,
                               static_cast<float>(getHeight() - scrollBarThickness));
            if (x - lastLabel >= 45) {
                g.setColour(juce::Colours::white.withAlpha(0.75F));
                g.drawText(juce::String(line.position.bar.value + 1) + "|" + juce::String(line.position.beat.value + 1),
                    static_cast<int>(x + 3), toolbarHeight, 80, 17, juce::Justification::centredLeft);
                lastLabel = x;
            }
        }
    } else for (auto seconds = std::max(0.0, first); seconds <= visibleEnd + tick; seconds += tick) {
        const auto x = static_cast<float>(headerWidth +
            (seconds - transform_.visibleStartSeconds()) * pixels);
        g.setColour(juce::Colours::white.withAlpha(0.22F));
        g.drawVerticalLine(static_cast<int>(std::round(x)), static_cast<float>(toolbarHeight + 17),
                           static_cast<float>(getHeight() - scrollBarThickness));
        g.setColour(juce::Colours::white.withAlpha(0.75F));
        g.drawText(rulerMode_.getSelectedId() == 2 ?
                   juce::String(static_cast<juce::int64>(std::llround(seconds * snapshot_.projectSampleRate.hertz()))) :
                   juce::String(seconds, tick < 1.0 ? 1 : 0) + " s",
                   static_cast<int>(x + 3), toolbarHeight, 58, 17,
                   juce::Justification::centredLeft);
    }

    for (std::size_t track = 0; track < snapshot_.tracks.size(); ++track) {
        const auto y = toolbarHeight + rulerHeight + static_cast<int>(track * laneHeight - verticalOffset_);
        if (y + laneHeight < viewport.getY() || y > viewport.getBottom()) continue;
        g.setColour(track % 2 == 0 ? juce::Colour{0xff252a31} : juce::Colour{0xff21262c});
        g.fillRect(0, y, getWidth() - scrollBarThickness, laneHeight);
        g.setColour(juce::Colour{0xff343b45});
        g.drawHorizontalLine(y + laneHeight - 1, 0.0F, static_cast<float>(getWidth()));
        g.setColour(juce::Colours::white.withAlpha(0.88F));
        g.setFont(juce::FontOptions{13.0F});
        g.drawText(juce::String(static_cast<int>(track + 1)) + "  " +
                       juce::String(snapshot_.tracks[track].name),
                   8, y, headerWidth - 12, laneHeight, juce::Justification::centredLeft, true);
        for (const auto& clip : snapshot_.tracks[track].clips) {
            const auto bounds = clipBounds(track, clip);
            if (!bounds.intersects(viewport.toFloat())) continue; // linear ordered culling, no component tree
            const auto selected = interaction_.selection() && *interaction_.selection() == clip.id;
            g.setColour(selected ? juce::Colour{0xffe5a84b} : juce::Colour{0xff4c87b9});
            g.fillRoundedRectangle(bounds, 4.0F);
            g.setColour(selected ? juce::Colours::white : juce::Colours::white.withAlpha(0.82F));
            g.drawRoundedRectangle(bounds, 4.0F, selected ? 2.0F : 1.0F);
            g.setColour(juce::Colours::black.withAlpha(0.25F));
            g.fillRect(bounds.withWidth(std::min(7.0F, bounds.getWidth())));
            g.fillRect(bounds.withLeft(std::max(bounds.getX(), bounds.getRight() - 7.0F)));
            if (bounds.getWidth() > 26.0F) {
                g.setColour(juce::Colours::white);
                g.drawFittedText(clip.label, bounds.toNearestInt().reduced(9, 2),
                                 juce::Justification::centredLeft, 1);
            }
        }
    }

    const auto playheadX = static_cast<float>(headerWidth +
        transform_.projectFrameToX(transport_.position));
    if (playheadX >= headerWidth && playheadX <= viewport.getRight()) {
        g.setColour(juce::Colour{0xffff554d});
        g.drawVerticalLine(static_cast<int>(std::round(playheadX)),
                           static_cast<float>(toolbarHeight),
                           static_cast<float>(getHeight() - scrollBarThickness));
    }
    g.restoreState();
}

void TimelineComponent::resized() {
    auto toolbar = getLocalBounds().removeFromTop(toolbarHeight).reduced(4, 3);
    toolbar.removeFromLeft(145);
    zoomOut_.setBounds(toolbar.removeFromLeft(30));
    zoomIn_.setBounds(toolbar.removeFromLeft(30));
    toolbar.removeFromLeft(10);
    split_.setBounds(toolbar.removeFromLeft(130));
    duplicate_.setBounds(toolbar.removeFromLeft(95));
    delete_.setBounds(toolbar.removeFromLeft(75));
    rulerMode_.setBounds(toolbar.removeFromLeft(135));
    horizontal_.setBounds(headerWidth, getHeight() - scrollBarThickness,
                          std::max(0, getWidth() - headerWidth - scrollBarThickness), scrollBarThickness);
    vertical_.setBounds(getWidth() - scrollBarThickness, toolbarHeight + rulerHeight,
                        scrollBarThickness,
                        std::max(0, getHeight() - toolbarHeight - rulerHeight - scrollBarThickness));
    updateScrollBars();
}

void TimelineComponent::updateScrollBars() {
    const auto viewport = viewportBounds();
    const auto visibleSeconds = viewport.getWidth() > 0
        ? viewport.getWidth() / transform_.pixelsPerSecond() : 1.0;
    const auto duration = snapshot_.projectSampleRate.isValid()
        ? static_cast<double>(std::max<std::int64_t>(0, snapshot_.contentDuration.value)) /
              snapshot_.projectSampleRate.hertz() : 0.0;
    const auto totalSeconds = std::max(30.0, duration + 10.0);
    horizontal_.setRangeLimits(0.0, totalSeconds);
    horizontal_.setCurrentRange(std::min(transform_.visibleStartSeconds(),
        std::max(0.0, totalSeconds - visibleSeconds)), std::min(totalSeconds, visibleSeconds));
    transform_.setVisibleStartSeconds(horizontal_.getCurrentRangeStart());
    const auto totalHeight = static_cast<double>(snapshot_.tracks.size() * laneHeight);
    const auto visibleHeight = static_cast<double>(std::max(1, viewport.getHeight()));
    vertical_.setRangeLimits(0.0, std::max(totalHeight, visibleHeight));
    vertical_.setCurrentRange(std::min(verticalOffset_, std::max(0.0, totalHeight - visibleHeight)),
                              std::min(totalHeight, visibleHeight));
    verticalOffset_ = vertical_.getCurrentRangeStart();
}

void TimelineComponent::setZoom(double zoom, double anchorX) {
    transform_.zoomAround(zoom, std::max(0.0, anchorX));
    updateScrollBars();
    repaint();
}

void TimelineComponent::dispatch(std::optional<commands::Command> command) {
    if (!command) {
        repaint();
        return;
    }
    const auto result = dispatcher_.dispatch(*command);
    interaction_.cancelGesture();
    refreshModel(false); // model is authoritative for success and failure
    repaint();
    if (commandCompleted) commandCompleted(result);
}

void TimelineComponent::mouseDown(const juce::MouseEvent& event) {
    grabKeyboardFocus();
    if (transport_.playback == transport::PlaybackState::playing) return;
    if (event.position.y >= toolbarHeight &&
        event.position.y < toolbarHeight + rulerHeight &&
        event.position.x >= headerWidth &&
        event.position.x <= viewportBounds().getRight()) {
        const auto target = transform_.xToProjectFrame(event.position.x - headerWidth);
        dispatch(commands::SeekToProjectFrame{target});
        return;
    }
    const auto hit = timelineHitTest(event.position);
    if (!hit.clip) {
        interaction_.select({});
        repaint();
        return;
    }
    interaction_.select(hit.clip->id);
    static_cast<void>(interaction_.beginGesture(hit.kind, *hit.clip,
                                                event.position.x - headerWidth));
    repaint();
}
void TimelineComponent::mouseDrag(const juce::MouseEvent& event) {
    if (transport_.playback == transport::PlaybackState::playing) return;
    interaction_.updateGesture(event.position.x - headerWidth, transform_);
    repaint();
}
void TimelineComponent::mouseUp(const juce::MouseEvent&) {
    dispatch(interaction_.endGesture());
}
void TimelineComponent::mouseWheelMove(const juce::MouseEvent& event,
                                       const juce::MouseWheelDetails& wheel) {
    if (event.mods.isCommandDown() || event.mods.isCtrlDown()) {
        setZoom(transform_.pixelsPerSecond() * std::pow(1.35, wheel.deltaY * 3.0),
                event.position.x - headerWidth);
    } else if (event.mods.isShiftDown() || std::abs(wheel.deltaX) > std::abs(wheel.deltaY)) {
        horizontal_.setCurrentRangeStart(horizontal_.getCurrentRangeStart() -
                                          (wheel.deltaX + wheel.deltaY) * 3.0);
    } else {
        vertical_.setCurrentRangeStart(vertical_.getCurrentRangeStart() - wheel.deltaY * laneHeight * 3.0);
    }
}
bool TimelineComponent::keyPressed(const juce::KeyPress& key) {
    if (key.getKeyCode() == juce::KeyPress::spaceKey) {
        dispatch(transport_.playback == transport::PlaybackState::playing
                     ? std::optional<commands::Command>{commands::Pause{}}
                     : std::optional<commands::Command>{commands::Play{}});
        return true;
    }
    if (key.getKeyCode() == juce::KeyPress::homeKey) {
        dispatch(commands::GoToStart{}); return true;
    }
    if (key.getKeyCode() == juce::KeyPress::endKey) {
        dispatch(commands::GoToEnd{}); return true;
    }
    if (key.getModifiers().isCommandDown() && key.getKeyCode() == 'Z') {
        dispatch(key.getModifiers().isShiftDown() ? std::optional<commands::Command>{commands::Redo{}}
                                                  : std::optional<commands::Command>{commands::Undo{}});
        return true;
    }
    if (key.getModifiers().isCommandDown() && key.getKeyCode() == 'D') {
        dispatch(interaction_.duplicateCommand(snapshot_)); return true;
    }
    if (key.getKeyCode() == juce::KeyPress::deleteKey || key.getKeyCode() == juce::KeyPress::backspaceKey) {
        dispatch(interaction_.deleteCommand()); return true;
    }
    if (key.getTextCharacter() == 's' || key.getTextCharacter() == 'S') {
        dispatch(interaction_.splitCommand(snapshot_, transport_.position)); return true;
    }
    return false;
}
void TimelineComponent::scrollBarMoved(juce::ScrollBar* bar, double start) {
    if (bar == &horizontal_) transform_.setVisibleStartSeconds(start);
    else if (bar == &vertical_) verticalOffset_ = std::max(0.0, start);
    repaint();
}
} // namespace vitadaw::platform::juce_adapter
