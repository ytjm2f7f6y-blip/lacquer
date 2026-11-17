cmake_minimum_required(VERSION 3.22)

project(Lacquer VERSION 0.4.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(LQ_BUILD_PLUGIN "Build the plugin (downloads JUCE)" ON)
option(LQ_BUILD_TESTS "Build the engine and plugin tests" ON)

# The engine: header-only, no dependencies.
add_library(lq_engine INTERFACE)
target_include_directories(lq_engine INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/engine)

if(LQ_BUILD_TESTS)
    enable_testing()
    add_executable(engine_tests tests/engine_tests.cpp)
    target_include_directories(engine_tests PRIVATE tests)
    target_link_libraries(engine_tests PRIVATE lq_engine)
    if(NOT MSVC)
        target_compile_options(engine_tests PRIVATE -O2 -Wall -Wextra)
    endif()
    add_test(NAME engine_tests COMMAND engine_tests)
endif()

if(LQ_BUILD_PLUGIN)
    include(FetchContent)
    if(DEFINED ENV{JUCE_DIR} AND EXISTS "$ENV{JUCE_DIR}/CMakeLists.txt")
        add_subdirectory($ENV{JUCE_DIR} ${CMAKE_BINARY_DIR}/juce)
    else()
        FetchContent_Declare(JUCE
            GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
            GIT_TAG 9.0.4
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(JUCE)
    endif()

    # An Audio Unit for Logic. (Elsewhere a VST3 is built, only so the code can be checked.)
    if(APPLE)
        set(LQ_FORMATS AU)
    else()
        set(LQ_FORMATS VST3)
    endif()

    juce_add_plugin(Lacquer
        PRODUCT_NAME "Lacquer"
        PLUGIN_NAME "Lacquer"
        COMPANY_NAME "Rumpus Room Records"
        BUNDLE_ID com.rumpusroomrecords.lacquer
        PLUGIN_MANUFACTURER_CODE SamP
        PLUGIN_CODE Lcqr
        FORMATS ${LQ_FORMATS}
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT FALSE
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        COPY_PLUGIN_AFTER_BUILD FALSE
        AU_MAIN_TYPE kAudioUnitType_Effect
        VST3_CATEGORIES Fx Mastering)

    # The rendered faceplate, tray and parts (art/), and the fonts: Caveat Brush and
    # Barlow Semi Condensed (SIL Open Font License, plugin/fonts).
    file(GLOB LQ_ASSETS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/plugin/assets/*)
    juce_add_binary_data(LacquerData SOURCES
        ${LQ_ASSETS}
        plugin/fonts/CaveatBrush-Regular.ttf
        plugin/fonts/BarlowSemiCondensed-SemiBold.ttf)
    set_target_properties(LacquerData PROPERTIES POSITION_INDEPENDENT_CODE TRUE)

    set(LQ_PLUGIN_SOURCES plugin/PluginProcessor.cpp plugin/PluginEditor.cpp plugin/Codec.cpp)
    set(LQ_DEFINES JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_DISPLAY_SPLASH_SCREEN=0)

    target_sources(Lacquer PRIVATE ${LQ_PLUGIN_SOURCES})
    target_include_directories(Lacquer PRIVATE plugin)
    target_compile_definitions(Lacquer PUBLIC ${LQ_DEFINES})
    target_link_libraries(Lacquer
        PRIVATE
            lq_engine
            LacquerData
            juce::juce_audio_utils
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags)

    if(LQ_BUILD_TESTS)
        # End-to-end: the real processor, run the way a host runs it.
        juce_add_console_app(plugin_selftest PRODUCT_NAME "plugin_selftest")
        target_sources(plugin_selftest PRIVATE tests/plugin_selftest.cpp ${LQ_PLUGIN_SOURCES})
        target_include_directories(plugin_selftest PRIVATE plugin tests)
        target_compile_definitions(plugin_selftest PRIVATE ${LQ_DEFINES} JucePlugin_VersionString="${PROJECT_VERSION}")
        target_link_libraries(plugin_selftest PRIVATE lq_engine LacquerData juce::juce_audio_utils juce::juce_recommended_config_flags)
        add_test(NAME plugin_selftest COMMAND plugin_selftest)

        # Runs finished songs through the analysis and the assistant, for checking it on real records.
        juce_add_console_app(lacquer_analyse PRODUCT_NAME "lacquer_analyse")
        target_sources(lacquer_analyse PRIVATE tests/analyse.cpp)
        target_compile_definitions(lacquer_analyse PRIVATE ${LQ_DEFINES})
        target_link_libraries(lacquer_analyse PRIVATE lq_engine juce::juce_audio_formats juce::juce_recommended_config_flags)

        # Renders the window to PNGs, for checking it without a host.
        juce_add_console_app(ui_snapshot PRODUCT_NAME "ui_snapshot")
        target_sources(ui_snapshot PRIVATE tests/ui_snapshot.cpp ${LQ_PLUGIN_SOURCES})
        target_include_directories(ui_snapshot PRIVATE plugin tests)
        target_compile_definitions(ui_snapshot PRIVATE ${LQ_DEFINES} JucePlugin_VersionString="${PROJECT_VERSION}")
        target_link_libraries(ui_snapshot PRIVATE lq_engine LacquerData juce::juce_audio_utils juce::juce_recommended_config_flags)
    endif()
endif()
