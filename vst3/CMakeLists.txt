cmake_minimum_required(VERSION 3.22)
project(LAN_MIC_VST3 VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)
set(SOS_JUCE_PATH "" CACHE PATH "Optional existing JUCE checkout")
if(SOS_JUCE_PATH)
    add_subdirectory("${SOS_JUCE_PATH}" "${CMAKE_BINARY_DIR}/JUCE")
else()
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.6
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
FetchContent_Declare(opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG v1.6.1
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(opus)

# Install the built bundles into the DAW plugin directory after every build.
# The default destination comes from JUCE: ~/Library/Audio/Plug-Ins/VST3 on
# macOS and C:/Program Files/Common Files/VST3 on Windows.
option(SOS_COPY_VST3_AFTER_BUILD "Copy the VST3 bundles into the plugin directory after each build" ON)
set(SOS_VST3_DIR "" CACHE PATH "VST3 install directory; empty uses the JUCE default")

if(SOS_COPY_VST3_AFTER_BUILD)
    set(sos_copy_args COPY_PLUGIN_AFTER_BUILD TRUE)
else()
    set(sos_copy_args COPY_PLUGIN_AFTER_BUILD FALSE)
endif()
if(SOS_VST3_DIR)
    list(APPEND sos_copy_args VST3_COPY_DIR "${SOS_VST3_DIR}")
endif()

function(add_bridge_plugin target code sending product_name bundle_id)
    if(sending)
        set(sos_vst3_categories Fx Network)
    else()
        # The receiver produces audio without an input signal, so hosts must
        # keep processing it like a generator even while the track is silent.
        set(sos_vst3_categories Fx Generator Network)
    endif()
    juce_add_plugin(${target}
        COMPANY_NAME "suxitech"
        BUNDLE_ID "${bundle_id}"
        PLUGIN_MANUFACTURER_CODE SoSu
        PLUGIN_CODE ${code}
        PRODUCT_NAME "${product_name}"
        FORMATS VST3
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT FALSE
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        ${sos_copy_args}
        VST3_CATEGORIES ${sos_vst3_categories})
    target_sources(${target} PRIVATE
        src/PluginProcessor.cpp src/PluginEditor.cpp src/Transport.cpp)
    target_include_directories(${target} PRIVATE src ../common)
    target_compile_definitions(${target} PUBLIC
        SOS_SENDING=${sending}
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)
    target_link_libraries(${target} PRIVATE
        juce::juce_audio_processors juce::juce_gui_extra opus
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
endfunction()

add_bridge_plugin(lan_mic_send LmSd 1 "LAN MIC Send" "cn.suxitech.lanmic.send")
add_bridge_plugin(lan_mic_return LmRt 0 "LAN MIC Return" "cn.suxitech.lanmic.return")
