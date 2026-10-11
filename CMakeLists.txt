cmake_minimum_required(VERSION 3.22)
project(Mender VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Link the Microsoft runtime statically so the Windows build runs on a studio PC
# without installing anything else.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(MENDER_BUILD_APP "Build the standalone editor (needs JUCE)" ON)
option(MENDER_DEV_TOOLS "Add the --snapshot command line mode used for automated checks" OFF)
set(MENDER_JUCE_PATH "" CACHE PATH "Path to a local JUCE checkout. Empty: download JUCE 9.0.3")

# --- Repair engine: plain C++, no dependencies ------------------------------
add_library(mender_engine STATIC engine/SpectralRepair.cpp)
target_include_directories(mender_engine PUBLIC engine)

enable_testing()
add_executable(mender_engine_tests tests/EngineTests.cpp)
target_link_libraries(mender_engine_tests PRIVATE mender_engine)
add_test(NAME engine COMMAND mender_engine_tests)

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
