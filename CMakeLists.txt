cmake_minimum_required(VERSION 3.24)

project(VitaDAW VERSION 0.0.4 LANGUAGES C CXX)

option(VITADAW_BUILD_TESTS "Build VitaDAW unit tests" ON)
option(VITADAW_BUILD_APP "Build the native JUCE application" ON)
option(VITADAW_FETCH_JUCE "Download the pinned JUCE version when it is not installed" ON)

add_library(vitadaw_core
    src/vitadaw/application/DawApplication.cpp
    src/vitadaw/audio/AudioDeviceState.cpp
    src/vitadaw/commands/CommandDispatcher.cpp
    src/vitadaw/project/ProjectState.cpp
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
endif()
