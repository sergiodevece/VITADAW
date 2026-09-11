cmake_minimum_required(VERSION 3.24)

project(VitaDAW VERSION 0.2.1 LANGUAGES C CXX)

option(VITADAW_BUILD_TESTS "Build VitaDAW unit tests" ON)
option(VITADAW_BUILD_APP "Build the native JUCE application" ON)
option(VITADAW_FETCH_JUCE "Download the pinned JUCE version when it is not installed" ON)

add_library(vitadaw_core
    src/vitadaw/application/DawApplication.cpp
    src/vitadaw/audio/AudioDeviceState.cpp
    src/vitadaw/audio/PreparedProcessingPlan.cpp
    src/vitadaw/audio/RealtimeAudioEngine.cpp
    src/vitadaw/commands/CommandDispatcher.cpp
    src/vitadaw/project/ProjectState.cpp
    src/vitadaw/routing/RoutingState.cpp
    src/vitadaw/timeline/Time.cpp
)

target_include_directories(vitadaw_core PUBLIC src)
target_compile_features(vitadaw_core PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(vitadaw_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(vitadaw_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(VITADAW_BUILD_APP)
    find_package(JUCE 9.0.2 EXACT CONFIG QUIET)

    if(NOT TARGET juce::juce_gui_basics)
        if(NOT VITADAW_FETCH_JUCE)
            message(FATAL_ERROR
                "JUCE 9.0.2 was not found. Set JUCE_DIR or enable VITADAW_FETCH_JUCE.")
        endif()

        include(FetchContent)
        FetchContent_Declare(
            JUCE
            GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
            GIT_TAG 9.0.2
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(JUCE)
    endif()

    juce_add_gui_app(vitadaw_app
        PRODUCT_NAME "VitaDAW"
        VERSION "${PROJECT_VERSION}"
    )

    target_sources(vitadaw_app PRIVATE
        src/vitadaw/platform/juce/JuceApplication.cpp
        src/vitadaw/platform/juce/JuceAudioDeviceAdapter.cpp
        src/vitadaw/platform/juce/JuceAudioDeviceAdapter.h
        src/vitadaw/platform/juce/MainWindow.cpp
        src/vitadaw/platform/juce/MainWindow.h
    )

    target_compile_features(vitadaw_app PRIVATE cxx_std_20)
    target_link_libraries(vitadaw_app PRIVATE
        vitadaw_core
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_gui_basics
    )
    target_compile_definitions(vitadaw_app PRIVATE
        JUCE_USE_FLAC=0
        JUCE_USE_MP3AUDIOFORMAT=0
        JUCE_USE_OGGVORBIS=0
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
    )

    if(MSVC)
        target_compile_options(vitadaw_app PRIVATE /W4 /permissive-)
    else()
        target_compile_options(vitadaw_app PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

if(VITADAW_BUILD_TESTS)
    enable_testing()

    add_executable(vitadaw_core_tests tests/CommandFlowTests.cpp)
    target_link_libraries(vitadaw_core_tests PRIVATE vitadaw_core)
    add_test(NAME vitadaw_core_tests COMMAND vitadaw_core_tests)

    add_executable(vitadaw_audio_device_state_tests tests/AudioDeviceStateTests.cpp)
    target_link_libraries(vitadaw_audio_device_state_tests PRIVATE vitadaw_core)
    add_test(NAME vitadaw_audio_device_state_tests COMMAND vitadaw_audio_device_state_tests)

    add_executable(vitadaw_playback_cursor_tests tests/PlaybackCursorTests.cpp)
    target_link_libraries(vitadaw_playback_cursor_tests PRIVATE vitadaw_core)
    add_test(NAME vitadaw_playback_cursor_tests COMMAND vitadaw_playback_cursor_tests)

    add_executable(vitadaw_time_conversion_tests tests/TimeConversionTests.cpp)
    target_link_libraries(vitadaw_time_conversion_tests PRIVATE vitadaw_core)
    add_test(NAME vitadaw_time_conversion_tests COMMAND vitadaw_time_conversion_tests)

    add_executable(vitadaw_transport_state_tests tests/TransportStateTests.cpp)
    target_link_libraries(vitadaw_transport_state_tests PRIVATE vitadaw_core)
    add_test(NAME vitadaw_transport_state_tests COMMAND vitadaw_transport_state_tests)

    add_executable(vitadaw_n_track_playback_tests tests/NTrackPlaybackTests.cpp)
    target_link_libraries(vitadaw_n_track_playback_tests PRIVATE vitadaw_core)
    add_test(NAME vitadaw_n_track_playback_tests COMMAND vitadaw_n_track_playback_tests)

    add_executable(vitadaw_mixer_core_tests tests/MixerCoreTests.cpp)
    target_link_libraries(vitadaw_mixer_core_tests PRIVATE vitadaw_core)
    add_test(NAME vitadaw_mixer_core_tests COMMAND vitadaw_mixer_core_tests)

    add_executable(vitadaw_smoothing_metering_tests
        tests/SmoothingMeteringTests.cpp)
    target_link_libraries(vitadaw_smoothing_metering_tests PRIVATE vitadaw_core)
    add_test(NAME vitadaw_smoothing_metering_tests
        COMMAND vitadaw_smoothing_metering_tests)

    add_executable(vitadaw_routing_foundation_tests
        tests/RoutingFoundationTests.cpp)
    target_link_libraries(vitadaw_routing_foundation_tests PRIVATE vitadaw_core)
    add_test(NAME vitadaw_routing_foundation_tests
        COMMAND vitadaw_routing_foundation_tests)

    add_executable(vitadaw_bus_mixer_controls_tests
        tests/BusMixerControlsTests.cpp)
    target_link_libraries(vitadaw_bus_mixer_controls_tests PRIVATE vitadaw_core)
    add_test(NAME vitadaw_bus_mixer_controls_tests
        COMMAND vitadaw_bus_mixer_controls_tests)

    add_executable(vitadaw_realtime_audio_engine_tests tests/RealtimeAudioEngineTests.cpp)
    target_link_libraries(vitadaw_realtime_audio_engine_tests PRIVATE vitadaw_core)
    add_test(NAME vitadaw_realtime_audio_engine_tests COMMAND vitadaw_realtime_audio_engine_tests)

    add_executable(vitadaw_audio_preparation_tests tests/AudioPreparationTests.cpp)
    target_link_libraries(vitadaw_audio_preparation_tests PRIVATE vitadaw_core)
    add_test(NAME vitadaw_audio_preparation_tests COMMAND vitadaw_audio_preparation_tests)

    add_executable(vitadaw_command_lifecycle_gate_tests
        tests/CommandLifecycleGateTests.cpp)
    target_link_libraries(vitadaw_command_lifecycle_gate_tests PRIVATE vitadaw_core)
    add_test(NAME vitadaw_command_lifecycle_gate_tests
        COMMAND vitadaw_command_lifecycle_gate_tests)

    add_executable(vitadaw_realtime_resource_lifetime_tests
        tests/RealtimeResourceLifetimeTests.cpp)
    target_link_libraries(vitadaw_realtime_resource_lifetime_tests PRIVATE vitadaw_core)
    add_test(NAME vitadaw_realtime_resource_lifetime_tests
        COMMAND vitadaw_realtime_resource_lifetime_tests)
endif()
