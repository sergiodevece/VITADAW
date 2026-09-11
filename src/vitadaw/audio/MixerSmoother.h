#pragma once

#include "vitadaw/audio/LinearSmoother.h"
#include "vitadaw/mixer/MixerState.h"

namespace vitadaw::audio {

inline constexpr double mixerSmoothingSeconds = 0.005;

class TrackMixSmoother {
public:
    void reset(mixer::PreparedTrackMixState state) noexcept {
        gain_.reset(state.linearGain);
        monoLeft_.reset(state.monoLeft);
        monoRight_.reset(state.monoRight);
        stereoLeft_.reset(state.stereoLeft);
        stereoRight_.reset(state.stereoRight);
        muted_ = state.muted;
        solo_ = state.solo;
    }

    void setTarget(mixer::PreparedTrackMixState state,
                   timeline::SampleRate deviceSampleRate) noexcept {
        gain_.setTarget(state.linearGain, deviceSampleRate,
                        mixerSmoothingSeconds);
        monoLeft_.setTarget(state.monoLeft, deviceSampleRate,
                            mixerSmoothingSeconds);
        monoRight_.setTarget(state.monoRight, deviceSampleRate,
                             mixerSmoothingSeconds);
        stereoLeft_.setTarget(state.stereoLeft, deviceSampleRate,
                              mixerSmoothingSeconds);
        stereoRight_.setTarget(state.stereoRight, deviceSampleRate,
                               mixerSmoothingSeconds);
        muted_ = state.muted;
        solo_ = state.solo;
    }

    [[nodiscard]] mixer::PreparedTrackMixState next() noexcept {
        return {gain_.next(), monoLeft_.next(), monoRight_.next(),
                stereoLeft_.next(), stereoRight_.next(), muted_, solo_};
    }

private:
    LinearSmoother gain_;
    LinearSmoother monoLeft_;
    LinearSmoother monoRight_;
    LinearSmoother stereoLeft_;
    LinearSmoother stereoRight_;
    bool muted_{};
    bool solo_{};
};

class MasterMixSmoother {
public:
    void reset(mixer::PreparedMasterMixState state) noexcept {
        gain_.reset(state.linearGain);
    }
    void setTarget(mixer::PreparedMasterMixState state,
                   timeline::SampleRate deviceSampleRate) noexcept {
        gain_.setTarget(state.linearGain, deviceSampleRate,
                        mixerSmoothingSeconds);
    }
    [[nodiscard]] mixer::PreparedMasterMixState next() noexcept {
        return {gain_.next()};
    }

private:
    LinearSmoother gain_;
};

class BusMixSmoother {
public:
    void reset(mixer::PreparedBusMixState state) noexcept {
        gain_.reset(state.linearGain);
        leftBalance_.reset(state.leftBalance);
        rightBalance_.reset(state.rightBalance);
        muted_ = state.muted;
        solo_ = state.solo;
    }
    void setTarget(mixer::PreparedBusMixState state,
                   timeline::SampleRate deviceSampleRate) noexcept {
        gain_.setTarget(state.linearGain, deviceSampleRate,
                        mixerSmoothingSeconds);
        leftBalance_.setTarget(state.leftBalance, deviceSampleRate,
                               mixerSmoothingSeconds);
        rightBalance_.setTarget(state.rightBalance, deviceSampleRate,
                                mixerSmoothingSeconds);
        muted_ = state.muted;
        solo_ = state.solo;
    }
    [[nodiscard]] mixer::PreparedBusMixState next() noexcept {
        return {gain_.next(), leftBalance_.next(), rightBalance_.next(),
                muted_, solo_};
    }

private:
    LinearSmoother gain_;
    LinearSmoother leftBalance_;
    LinearSmoother rightBalance_;
    bool muted_{};
    bool solo_{};
};

} // namespace vitadaw::audio
