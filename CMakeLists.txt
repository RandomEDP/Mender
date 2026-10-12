cmake_minimum_required(VERSION 3.22)
project(Mender VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Link the Microsoft runtime statically so the Windows build runs on a studio PC
# without installing anything else.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(MENDER_BUILD_APP "Build the standalone editor (needs JUCE)" ON)
option(MENDER_BUILD_PLUGIN "Build the VST3 + ARA plug-in (needs JUCE and the ARA SDK)" ON)
option(MENDER_DEV_TOOLS "Add the --snapshot command line mode used for automated checks" OFF)
set(MENDER_JUCE_PATH "" CACHE PATH "Path to a local JUCE checkout. Empty: download JUCE 9.0.3")
set(MENDER_ARA_SDK_PATH "" CACHE PATH "Path to a local ARA SDK checkout. Empty: download ARA SDK 2.3.0")

# --- Repair engine: plain C++, no dependencies ------------------------------
add_library(mender_engine STATIC engine/SpectralRepair.cpp engine/SpectralEdit.cpp)
target_include_directories(mender_engine PUBLIC engine)
set_target_properties(mender_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)

enable_testing()
add_executable(mender_engine_tests tests/EngineTests.cpp)
target_link_libraries(mender_engine_tests PRIVATE mender_engine)
add_test(NAME engine COMMAND mender_engine_tests)
add_executable(mender_edit_tests tests/EditTests.cpp)
target_link_libraries(mender_edit_tests PRIVATE mender_engine)
add_test(NAME edits COMMAND mender_edit_tests)

if(NOT MENDER_BUILD_APP)
    return()
endif()

# --- JUCE --------------------------------------------------------------------
if(MENDER_JUCE_PATH)
    add_subdirectory(${MENDER_JUCE_PATH} juce EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(juce
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 9.0.3
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(juce)
endif()

# --- Standalone editor -------------------------------------------------------
juce_add_gui_app(Mender
    PRODUCT_NAME "Mender"
    COMPANY_NAME "Mender"
    BUNDLE_ID "com.mender.editor"
    DOCUMENT_EXTENSIONS wav aif aiff flac)

target_sources(Mender PRIVATE
    app/Main.cpp
    app/MainComponent.cpp
    app/ClipPlayer.cpp
    app/WavChunks.cpp
    app/SpectrogramView.cpp)

target_compile_definitions(Mender PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:Mender,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:Mender,JUCE_VERSION>"
    MENDER_DEV_TOOLS=$<BOOL:${MENDER_DEV_TOOLS}>)

target_link_libraries(Mender
    PRIVATE
        mender_engine
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

if(NOT MENDER_BUILD_PLUGIN)
    return()
endif()

# --- Plug-in: VST3 with the ARA2 extension -----------------------------------
if(MENDER_ARA_SDK_PATH)
    set(ara_sdk_SOURCE_DIR ${MENDER_ARA_SDK_PATH})
else()
    include(FetchContent)
    FetchContent_Declare(ara_sdk
        GIT_REPOSITORY https://github.com/Celemony/ARA_SDK.git
        GIT_TAG releases/2.3.0
        GIT_SHALLOW TRUE
        GIT_SUBMODULES ARA_API ARA_Library)
    FetchContent_MakeAvailable(ara_sdk)
endif()
juce_set_ara_sdk_path(${ara_sdk_SOURCE_DIR})

juce_add_plugin(MenderPlugin
    PRODUCT_NAME "Mender"
    COMPANY_NAME "Mender"
    COMPANY_WEBSITE "https://github.com/RandomEDP/Mender"
    BUNDLE_ID "com.mender.plugin"
    PLUGIN_MANUFACTURER_CODE Mndr
    PLUGIN_CODE Mnd1
    FORMATS VST3
    VST3_CATEGORIES Fx Restoration
    IS_ARA_EFFECT TRUE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(MenderPlugin PRIVATE
    plugin/AraDocument.cpp
    plugin/PatchList.cpp
    plugin/PluginEditor.cpp
    plugin/PluginProcessor.cpp
    app/SpectrogramView.cpp)

target_include_directories(MenderPlugin PRIVATE app plugin)

target_compile_definitions(MenderPlugin PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(MenderPlugin
    PRIVATE
        mender_engine
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

juce_add_console_app(mender_patch_tests)
target_sources(mender_patch_tests PRIVATE tests/PatchListTests.cpp plugin/PatchList.cpp)
target_link_libraries(mender_patch_tests PRIVATE juce::juce_audio_basics juce::juce_recommended_config_flags)
add_test(NAME patches COMMAND mender_patch_tests)

juce_add_console_app(mender_player_tests)
target_sources(mender_player_tests PRIVATE tests/PlayerTests.cpp app/ClipPlayer.cpp)
target_link_libraries(mender_player_tests PRIVATE juce::juce_audio_basics juce::juce_recommended_config_flags)
add_test(NAME player COMMAND mender_player_tests)
