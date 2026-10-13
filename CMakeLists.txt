cmake_minimum_required(VERSION 3.24)

project(VitaDAW VERSION 0.8.2 LANGUAGES C CXX)

include(FetchContent)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(nlohmann_json)

option(VITADAW_BUILD_TESTS "Build VitaDAW unit tests" ON)
option(VITADAW_BUILD_APP "Build the native JUCE application" ON)
option(VITADAW_FETCH_JUCE "Download the pinned JUCE version when it is not installed" ON)
option(VITADAW_BUILD_BENCHMARKS "Build VitaDAW non-gating benchmarks" OFF)
option(VITADAW_RECORDING_SERVICE_DIAGNOSTICS
    "Development only: measure recording writer service latency and allow injected service stalls"
    OFF)

add_library(vitadaw_core
    src/vitadaw/application/DawApplication.cpp
    src/vitadaw/application/ProjectPersistenceCommands.cpp
    src/vitadaw/application/MusicalTimeCommands.cpp
    src/vitadaw/musical/MusicalTime.cpp
    src/vitadaw/audio/AudioDeviceState.cpp
    src/vitadaw/audio/LoopbackLatency.cpp
    src/vitadaw/audio/OfflineRenderer.cpp
    src/vitadaw/audio/PreparedProcessingPlan.cpp
    src/vitadaw/audio/PreparedTemporalContext.cpp
    src/vitadaw/audio/RealtimeCapture.cpp
    src/vitadaw/audio/RecordingPlacement.cpp
    src/vitadaw/audio/RecordingRecovery.cpp
    src/vitadaw/audio/RealtimeAudioEngine.cpp
    src/vitadaw/commands/CommandDispatcher.cpp
    src/vitadaw/history/UndoManager.cpp
    src/vitadaw/project/ProjectState.cpp
    src/vitadaw/project/ProjectDocumentData.cpp
    src/vitadaw/persistence/ProjectPersistence.cpp
    src/vitadaw/platform/files/ProjectFileIO.cpp
    src/vitadaw/platform/files/RecoveryInventory.cpp
    src/vitadaw/platform/files/RecordingMediaIO.cpp
    src/vitadaw/processors/GainProcessor.cpp
    src/vitadaw/routing/RoutingState.cpp
    src/vitadaw/timeline/Time.cpp
    src/vitadaw/ui/timeline/TimelineModel.cpp
    src/vitadaw/ui/timeline/WaveformView.cpp
    src/vitadaw/waveform/WaveformCache.cpp
)

target_include_directories(vitadaw_core PUBLIC src)
target_compile_features(vitadaw_core PUBLIC cxx_std_20)
target_link_libraries(vitadaw_core PRIVATE nlohmann_json::nlohmann_json)

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
        MICROPHONE_PERMISSION_ENABLED TRUE
        MICROPHONE_PERMISSION_TEXT "VitaDAW needs microphone access to record audio."
    )

    target_sources(vitadaw_app PRIVATE
        src/vitadaw/platform/juce/JuceApplication.cpp
        src/vitadaw/platform/juce/JuceAudioDeviceAdapter.cpp
        src/vitadaw/platform/juce/JuceAudioDeviceAdapter.h
        src/vitadaw/platform/juce/MainWindow.cpp
        src/vitadaw/platform/juce/MainWindow.h
        src/vitadaw/platform/juce/TimelineComponent.cpp
        src/vitadaw/platform/juce/TimelineComponent.h
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
    if(VITADAW_RECORDING_SERVICE_DIAGNOSTICS)
        message(WARNING
            "VITADAW_RECORDING_SERVICE_DIAGNOSTICS is ON: this application build measures "
            "recording service latency and honours VITADAW_DIAG_SERVICE_STALLS_MS. "
            "Development use only - never distribute this build.")
        target_compile_definitions(vitadaw_app PRIVATE VITADAW_RECORDING_SERVICE_DIAGNOSTICS=1)
    endif()

    if(MSVC)
        target_compile_options(vitadaw_app PRIVATE /W4 /permissive-)
    else()
        target_compile_options(vitadaw_app PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

if(VITADAW_BUILD_BENCHMARKS)
    add_executable(vitadaw_arrange_rebuild_benchmarks
        tests/ArrangeRebuildBenchmarks.cpp)
    target_link_libraries(vitadaw_arrange_rebuild_benchmarks PRIVATE vitadaw_core)
endif()

if(VITADAW_BUILD_TESTS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_executable(vitadaw_temporal_integer_oracle tests/TemporalIntegerOracleDriver.cpp)
    target_link_libraries(vitadaw_temporal_integer_oracle PRIVATE vitadaw_core)
    add_test(NAME vitadaw_temporal_integer_oracle
        COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/TemporalIntegerOracleTests.py
                $<TARGET_FILE:vitadaw_temporal_integer_oracle>)
    add_executable(vitadaw_temporal_portable_oracle tests/TemporalIntegerOracleDriver.cpp)
    target_link_libraries(vitadaw_temporal_portable_oracle PRIVATE vitadaw_core)
    target_compile_definitions(vitadaw_temporal_portable_oracle PRIVATE VITADAW_FORCE_PORTABLE_TEMPORAL=1)
    add_test(NAME vitadaw_temporal_portable_oracle
        COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/TemporalIntegerOracleTests.py
                $<TARGET_FILE:vitadaw_temporal_portable_oracle>)
    if(NOT MSVC)
        add_executable(vitadaw_temporal_fastmath_oracle tests/TemporalIntegerOracleDriver.cpp)
        target_link_libraries(vitadaw_temporal_fastmath_oracle PRIVATE vitadaw_core)
        target_compile_options(vitadaw_temporal_fastmath_oracle PRIVATE -O2 -ffast-math -ffp-contract=fast)
        add_test(NAME vitadaw_temporal_fastmath_oracle
            COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/TemporalIntegerOracleTests.py
                    $<TARGET_FILE:vitadaw_temporal_fastmath_oracle>)
    endif()
    add_executable(vitadaw_musical_preparation_oracle
        tests/MusicalPreparationOracleDriver.cpp
        src/vitadaw/musical/MusicalTime.cpp
        src/vitadaw/timeline/Time.cpp)
    target_include_directories(vitadaw_musical_preparation_oracle PRIVATE src)
    target_compile_features(vitadaw_musical_preparation_oracle PRIVATE cxx_std_20)
    add_test(NAME vitadaw_musical_preparation_oracle
        COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/MusicalPreparationOracleTests.py
                $<TARGET_FILE:vitadaw_musical_preparation_oracle>)
    add_executable(vitadaw_musical_preparation_portable_oracle
        tests/MusicalPreparationOracleDriver.cpp
        src/vitadaw/musical/MusicalTime.cpp
        src/vitadaw/timeline/Time.cpp)
    target_include_directories(vitadaw_musical_preparation_portable_oracle PRIVATE src)
    target_compile_features(vitadaw_musical_preparation_portable_oracle PRIVATE cxx_std_20)
    target_compile_definitions(vitadaw_musical_preparation_portable_oracle PRIVATE
        VITADAW_FORCE_PORTABLE_TEMPORAL=1)
    add_test(NAME vitadaw_musical_preparation_portable_oracle
        COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/MusicalPreparationOracleTests.py
                $<TARGET_FILE:vitadaw_musical_preparation_portable_oracle>)
    if(NOT MSVC)
        add_executable(vitadaw_musical_preparation_fastmath_oracle
            tests/MusicalPreparationOracleDriver.cpp
            src/vitadaw/musical/MusicalTime.cpp
            src/vitadaw/timeline/Time.cpp)
        target_include_directories(vitadaw_musical_preparation_fastmath_oracle PRIVATE src)
        target_compile_features(vitadaw_musical_preparation_fastmath_oracle PRIVATE cxx_std_20)
        target_compile_options(vitadaw_musical_preparation_fastmath_oracle PRIVATE
            -O2 -ffast-math -ffp-contract=fast)
        add_test(NAME vitadaw_musical_preparation_fastmath_oracle
            COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/MusicalPreparationOracleTests.py
                    $<TARGET_FILE:vitadaw_musical_preparation_fastmath_oracle>)
    endif()
    add_executable(vitadaw_loop_metronome_tests tests/LoopMetronomeTests.cpp)
    target_link_libraries(vitadaw_loop_metronome_tests PRIVATE vitadaw_core)
    add_test(NAME vitadaw_loop_metronome_tests COMMAND vitadaw_loop_metronome_tests)
    add_executable(vitadaw_musical_time_tests tests/MusicalTimeTests.cpp)
    target_link_libraries(vitadaw_musical_time_tests PRIVATE vitadaw_core)
    add_test(NAME vitadaw_musical_time_tests COMMAND vitadaw_musical_time_tests)
    enable_testing()
    add_executable(vitadaw_project_persistence_tests tests/ProjectPersistenceTests.cpp)
    target_link_libraries(vitadaw_project_persistence_tests PRIVATE vitadaw_core nlohmann_json::nlohmann_json)
    target_compile_definitions(vitadaw_project_persistence_tests PRIVATE
        VITADAW_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/persistence")
    add_test(NAME vitadaw_project_persistence_tests COMMAND vitadaw_project_persistence_tests)
    if(VITADAW_BUILD_APP)
        add_executable(vitadaw_project_media_integration_tests
            tests/ProjectMediaIntegrationTests.cpp
            src/vitadaw/platform/juce/JuceAudioDeviceAdapter.cpp)
        target_link_libraries(vitadaw_project_media_integration_tests PRIVATE vitadaw_core
            juce::juce_audio_devices juce::juce_audio_formats)
        target_compile_definitions(vitadaw_project_media_integration_tests PRIVATE
            JUCE_USE_FLAC=0 JUCE_USE_MP3AUDIOFORMAT=0 JUCE_USE_OGGVORBIS=0 JUCE_USE_CURL=0)
        add_test(NAME vitadaw_project_media_integration_tests COMMAND vitadaw_project_media_integration_tests)
        add_executable(vitadaw_temporal_juce_integration_tests
            tests/TemporalJuceIntegrationTests.cpp
            src/vitadaw/platform/juce/JuceAudioDeviceAdapter.cpp)
        target_link_libraries(vitadaw_temporal_juce_integration_tests PRIVATE vitadaw_core
            juce::juce_audio_devices juce::juce_audio_formats)
        target_compile_definitions(vitadaw_temporal_juce_integration_tests PRIVATE
            JUCE_USE_FLAC=0 JUCE_USE_MP3AUDIOFORMAT=0 JUCE_USE_OGGVORBIS=0 JUCE_USE_CURL=0)
        foreach(case bootstrap lifetime checkpoint rollback metronome_rates metronome_rollback run_until_stop pending_wrap)
            add_test(NAME vitadaw_temporal_juce_${case}
                COMMAND vitadaw_temporal_juce_integration_tests ${case})
        endforeach()
        add_executable(vitadaw_juce_recording_input_setup_policy_tests
            tests/JuceRecordingInputSetupPolicyTests.cpp)
        target_include_directories(vitadaw_juce_recording_input_setup_policy_tests PRIVATE src)
        target_link_libraries(vitadaw_juce_recording_input_setup_policy_tests PRIVATE
            juce::juce_audio_devices)
        add_test(NAME vitadaw_juce_recording_input_setup_policy_tests
            COMMAND vitadaw_juce_recording_input_setup_policy_tests)
        # 0.8.3-C2-1 device-failure characterization: one CTest per case.
        # RED cases reproduce defects from audit 0.8.3-C1 and exit 3 until the
        # matching C2 change lands; run the green baseline with
        # `ctest -LE characterization-red`.
        add_executable(vitadaw_recording_device_failure_tests
            tests/RecordingDeviceFailureTests.cpp
            src/vitadaw/platform/juce/JuceAudioDeviceAdapter.cpp)
        target_link_libraries(vitadaw_recording_device_failure_tests PRIVATE vitadaw_core
            juce::juce_audio_devices juce::juce_audio_formats)
        target_compile_definitions(vitadaw_recording_device_failure_tests PRIVATE
            JUCE_USE_FLAC=0 JUCE_USE_MP3AUDIOFORMAT=0 JUCE_USE_OGGVORBIS=0 JUCE_USE_CURL=0)
        foreach(case
                t1_device_stop_fails_take_and_recovers
                t2a_juce_drops_device_errors
                t2b_direct_error_path_is_fail_safe
                t3_restart_fails_take_without_silent_continuation
                t4_divergence_fails_before_reprepare
                t5_buffer_change_rejected_while_capturing
                t8_device_loss_during_finalization
                c_rate_window_then_restart_fails_take
                e_suspected_xruns_do_not_abort
                e_missing_host_time_is_not_a_failure
                f_interface_loss_fails_take_and_leaves_no_device
                g1_stop_then_device_loss_publishes
                g2_device_loss_before_stop_is_consumed
                g3_overflow_then_device_loss
                g4_stop_pending_then_restart
                m1_stop_pending_stall_fails_take
                m1_sustained_stall_fails_take
                m1_cancel_pending_stall_fails_take
                m1_continuous_callbacks_never_trip
                m1_silent_input_is_not_a_stall
                m1_message_thread_delay_with_live_callbacks
                m1_late_callback_rearms_watchdog
                m1_concurrent_callbacks_then_stall
                m1_overflow_wins_over_watchdog
                m1_watchdog_and_stop_notification
                m1_shutdown_during_stall
                m1_device_resumes_after_stall_failure
                m1_short_pause_is_left_to_continuity_checks
                # 0.8.3-C2-2b: M-1b and P-1 (formerly RED), C-1 (formerly
                # labelled characterization-tsan; clean under ThreadSanitizer).
                m1b_prepared_stall_terminalizes
                p1_stale_begin_cannot_revive_cleaned_session
                c1_prepared_transition_under_concurrency
                m1b_prepared_stall_without_commands
                m1b_first_callback_rearms_watchdog
                m1b_slow_first_callback_is_not_a_stall
                m1b_device_resumes_after_prepared_failure
                stop_before_first_callback_leaves_nothing
                cycles_of_failure_and_recovery)
            add_test(NAME vitadaw_device_failure_${case}
                COMMAND vitadaw_recording_device_failure_tests ${case})
        endforeach()
        foreach(case
                red_h2_stop_inside_rate_window
                red_h1_host_time_gap
                red_r2_reconnected_interface_unusable)
            add_test(NAME vitadaw_device_failure_${case}
                COMMAND vitadaw_recording_device_failure_tests ${case})
            set_tests_properties(vitadaw_device_failure_${case} PROPERTIES
                LABELS characterization-red)
        endforeach()
        add_executable(vitadaw_recording_service_diagnostics_tests
            tests/RecordingServiceDiagnosticsTests.cpp)
        target_link_libraries(vitadaw_recording_service_diagnostics_tests PRIVATE vitadaw_core)
        add_test(NAME vitadaw_recording_service_diagnostics_tests
            COMMAND vitadaw_recording_service_diagnostics_tests)
        add_executable(vitadaw_input_monitoring_device_lifecycle_tests
            tests/InputMonitoringDeviceLifecycleTests.cpp
            src/vitadaw/platform/juce/JuceAudioDeviceAdapter.cpp)
        target_link_libraries(vitadaw_input_monitoring_device_lifecycle_tests PRIVATE vitadaw_core
            juce::juce_audio_devices juce::juce_audio_formats)
        target_compile_definitions(vitadaw_input_monitoring_device_lifecycle_tests PRIVATE
            JUCE_USE_FLAC=0 JUCE_USE_MP3AUDIOFORMAT=0 JUCE_USE_OGGVORBIS=0 JUCE_USE_CURL=0)
        add_test(NAME vitadaw_input_monitoring_device_lifecycle_tests
            COMMAND vitadaw_input_monitoring_device_lifecycle_tests)
        add_executable(vitadaw_audio_export_tests
            tests/AudioExportTests.cpp
            src/vitadaw/platform/juce/JuceAudioDeviceAdapter.cpp)
        target_link_libraries(vitadaw_audio_export_tests PRIVATE vitadaw_core
            juce::juce_audio_devices juce::juce_audio_formats)
        target_compile_definitions(vitadaw_audio_export_tests PRIVATE
            JUCE_USE_FLAC=0 JUCE_USE_MP3AUDIOFORMAT=0 JUCE_USE_OGGVORBIS=0 JUCE_USE_CURL=0)
        add_test(NAME vitadaw_audio_export_tests COMMAND vitadaw_audio_export_tests)
    endif()
    add_executable(vitadaw_undo_redo_tests tests/UndoRedoTests.cpp)
    target_link_libraries(vitadaw_undo_redo_tests PRIVATE vitadaw_core)
    add_test(NAME vitadaw_undo_redo_tests COMMAND vitadaw_undo_redo_tests)

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
    add_executable(vitadaw_recording_placement_tests tests/RecordingPlacementTests.cpp)
    target_link_libraries(vitadaw_recording_placement_tests PRIVATE vitadaw_core)
    add_test(NAME vitadaw_recording_placement_tests COMMAND vitadaw_recording_placement_tests)
    add_executable(vitadaw_loopback_latency_tests tests/LoopbackLatencyTests.cpp)
    target_link_libraries(vitadaw_loopback_latency_tests PRIVATE vitadaw_core)
    add_test(NAME vitadaw_loopback_latency_tests COMMAND vitadaw_loopback_latency_tests)

    add_executable(vitadaw_transport_state_tests tests/TransportStateTests.cpp)
    target_link_libraries(vitadaw_transport_state_tests PRIVATE vitadaw_core)
    add_test(NAME vitadaw_transport_state_tests COMMAND vitadaw_transport_state_tests)

    add_executable(vitadaw_n_track_playback_tests tests/NTrackPlaybackTests.cpp)
    target_link_libraries(vitadaw_n_track_playback_tests PRIVATE vitadaw_core)
    add_test(NAME vitadaw_n_track_playback_tests COMMAND vitadaw_n_track_playback_tests)

    add_executable(vitadaw_offline_renderer_tests tests/OfflineRendererTests.cpp)
    target_link_libraries(vitadaw_offline_renderer_tests PRIVATE vitadaw_core)
    add_test(NAME vitadaw_offline_renderer_tests COMMAND vitadaw_offline_renderer_tests)

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

    add_executable(vitadaw_bus_to_bus_routing_tests
        tests/BusToBusRoutingTests.cpp)
    target_link_libraries(vitadaw_bus_to_bus_routing_tests PRIVATE vitadaw_core)
    add_test(NAME vitadaw_bus_to_bus_routing_tests
        COMMAND vitadaw_bus_to_bus_routing_tests)

    add_executable(vitadaw_track_sends_auxes_tests
        tests/TrackSendsAuxesTests.cpp)
    target_link_libraries(vitadaw_track_sends_auxes_tests PRIVATE vitadaw_core)
    add_test(NAME vitadaw_track_sends_auxes_tests
        COMMAND vitadaw_track_sends_auxes_tests)

    add_executable(vitadaw_bus_sends_tests tests/BusSendsTests.cpp)
    target_link_libraries(vitadaw_bus_sends_tests PRIVATE vitadaw_core)
    add_test(NAME vitadaw_bus_sends_tests COMMAND vitadaw_bus_sends_tests)

    add_executable(vitadaw_realtime_audio_engine_tests tests/RealtimeAudioEngineTests.cpp)
    target_link_libraries(vitadaw_realtime_audio_engine_tests PRIVATE vitadaw_core)
    add_test(NAME vitadaw_realtime_audio_engine_tests COMMAND vitadaw_realtime_audio_engine_tests)

    add_executable(vitadaw_input_monitoring_tests tests/InputMonitoringTests.cpp)
    target_link_libraries(vitadaw_input_monitoring_tests PRIVATE vitadaw_core)
    add_test(NAME vitadaw_input_monitoring_tests COMMAND vitadaw_input_monitoring_tests)

    add_executable(vitadaw_recording_capture_tests tests/RecordingCaptureTests.cpp)
    target_link_libraries(vitadaw_recording_capture_tests PRIVATE vitadaw_core)
    add_test(NAME vitadaw_recording_capture_tests COMMAND vitadaw_recording_capture_tests)
    add_executable(vitadaw_recording_state_protocol_tests tests/RecordingStateProtocolTests.cpp)
    target_link_libraries(vitadaw_recording_state_protocol_tests PRIVATE vitadaw_core)
    add_test(NAME vitadaw_recording_state_protocol_tests
        COMMAND vitadaw_recording_state_protocol_tests)
    add_executable(vitadaw_recording_application_tests tests/RecordingApplicationTests.cpp)
    target_link_libraries(vitadaw_recording_application_tests PRIVATE vitadaw_core)
    add_test(NAME vitadaw_recording_application_tests COMMAND vitadaw_recording_application_tests)
    add_executable(vitadaw_recording_endurance_tests tests/RecordingEnduranceTests.cpp)
    target_link_libraries(vitadaw_recording_endurance_tests PRIVATE vitadaw_core)
    add_test(NAME vitadaw_recording_endurance_tests COMMAND vitadaw_recording_endurance_tests)
    add_executable(vitadaw_recording_recovery_tests tests/RecordingRecoveryTests.cpp)
    target_link_libraries(vitadaw_recording_recovery_tests PRIVATE vitadaw_core)
    add_test(NAME vitadaw_recording_recovery_tests COMMAND vitadaw_recording_recovery_tests)

    add_executable(vitadaw_recovery_inventory_tests tests/RecoveryInventoryTests.cpp)
    target_link_libraries(vitadaw_recovery_inventory_tests PRIVATE vitadaw_core)
    add_test(NAME vitadaw_recovery_inventory_tests COMMAND vitadaw_recovery_inventory_tests)

    add_executable(vitadaw_recording_media_io_tests tests/RecordingMediaIOTests.cpp)
    target_link_libraries(vitadaw_recording_media_io_tests PRIVATE vitadaw_core)
    add_test(NAME vitadaw_recording_media_io_tests COMMAND vitadaw_recording_media_io_tests)

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

    add_executable(vitadaw_processor_insert_core_tests
        tests/ProcessorInsertCoreTests.cpp)
    target_link_libraries(vitadaw_processor_insert_core_tests PRIVATE vitadaw_core)
    add_test(NAME vitadaw_processor_insert_core_tests
        COMMAND vitadaw_processor_insert_core_tests)

    add_executable(vitadaw_processor_command_tests
        tests/ProcessorCommandTests.cpp)
    target_link_libraries(vitadaw_processor_command_tests PRIVATE vitadaw_core)
    add_test(NAME vitadaw_processor_command_tests
        COMMAND vitadaw_processor_command_tests)

    add_executable(vitadaw_source_clip_track_foundation_tests
        tests/SourceClipTrackFoundationTests.cpp)
    target_link_libraries(vitadaw_source_clip_track_foundation_tests PRIVATE vitadaw_core)
    add_test(NAME vitadaw_source_clip_track_foundation_tests
        COMMAND vitadaw_source_clip_track_foundation_tests)

    add_executable(vitadaw_timeline_editing_operations_tests
        tests/TimelineEditingOperationsTests.cpp)
    target_link_libraries(vitadaw_timeline_editing_operations_tests PRIVATE vitadaw_core)
    add_test(NAME vitadaw_timeline_editing_operations_tests
        COMMAND vitadaw_timeline_editing_operations_tests)

    add_executable(vitadaw_timeline_ui_foundation_tests
        tests/TimelineUiFoundationTests.cpp)
    target_link_libraries(vitadaw_timeline_ui_foundation_tests PRIVATE vitadaw_core)
    add_test(NAME vitadaw_timeline_ui_foundation_tests
        COMMAND vitadaw_timeline_ui_foundation_tests)

    add_executable(vitadaw_track_operations_tests
        tests/TrackOperationsTests.cpp)
    target_link_libraries(vitadaw_track_operations_tests PRIVATE vitadaw_core)
    add_test(NAME vitadaw_track_operations_tests
        COMMAND vitadaw_track_operations_tests)

    add_executable(vitadaw_waveform_foundation_tests
        tests/WaveformFoundationTests.cpp)
    target_link_libraries(vitadaw_waveform_foundation_tests PRIVATE vitadaw_core)
    add_test(NAME vitadaw_waveform_foundation_tests
        COMMAND vitadaw_waveform_foundation_tests)

    add_executable(vitadaw_editing_transport_hardening_tests
        tests/EditingTransportHardeningTests.cpp)
    target_link_libraries(vitadaw_editing_transport_hardening_tests PRIVATE vitadaw_core)
    add_test(NAME vitadaw_editing_transport_hardening_tests
        COMMAND vitadaw_editing_transport_hardening_tests)

    add_executable(vitadaw_transport_timeline_foundation_tests
        tests/TransportTimelineFoundationTests.cpp)
    target_link_libraries(vitadaw_transport_timeline_foundation_tests PRIVATE vitadaw_core)
    add_test(NAME vitadaw_transport_timeline_foundation_tests
        COMMAND vitadaw_transport_timeline_foundation_tests)

    add_executable(vitadaw_application_lifecycle_tests
        tests/ApplicationLifecycleTests.cpp)
    target_link_libraries(vitadaw_application_lifecycle_tests PRIVATE vitadaw_core)
    add_test(NAME vitadaw_application_lifecycle_tests
        COMMAND vitadaw_application_lifecycle_tests)
endif()
