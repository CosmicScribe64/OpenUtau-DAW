cmake_minimum_required(VERSION 3.24)
project(OpenUtauVstNative VERSION 0.1.0 LANGUAGES C CXX)

include(FetchContent)
FetchContent_Declare(
  JUCE
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG 8.0.15
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(JUCE)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(openutau_audio_bridge STATIC
  src/audio_ring_buffer.cpp
  src/engine_bridge.cpp
  src/engine_client.cpp
)
set_target_properties(openutau_audio_bridge PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(openutau_audio_bridge PUBLIC include)
target_link_libraries(openutau_audio_bridge PUBLIC juce::juce_core)
target_compile_definitions(openutau_audio_bridge PUBLIC JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
target_compile_options(openutau_audio_bridge PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Werror>
)

enable_testing()
add_executable(audio_ring_buffer_tests tests/audio_ring_buffer_tests.cpp)
target_link_libraries(audio_ring_buffer_tests PRIVATE openutau_audio_bridge)
add_test(NAME audio_ring_buffer_tests COMMAND audio_ring_buffer_tests)

add_executable(engine_client_tests tests/engine_client_tests.cpp)
target_link_libraries(engine_client_tests PRIVATE openutau_audio_bridge)
add_test(NAME engine_client_tests COMMAND engine_client_tests
  "dotnet"
  "${CMAKE_SOURCE_DIR}/../.build/dotnet/bin/OpenUtau.Vst.Engine.Host/Release/net8.0/OpenUtau.Vst.Engine.Host.dll")
set_tests_properties(engine_client_tests PROPERTIES
  ENVIRONMENT "OPENUTAU_VST_EDITOR=${CMAKE_SOURCE_DIR}/tests/fake_editor.sh;OPENUTAU_VST_TEST_RENDER_DELAY_MS=200")

add_executable(fault_engine_host tests/fault_engine_host.cpp)
target_link_libraries(fault_engine_host PRIVATE juce::juce_core)
target_compile_definitions(fault_engine_host PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
target_compile_options(fault_engine_host PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Werror>
)

add_executable(engine_bridge_tests tests/engine_bridge_tests.cpp)
target_link_libraries(engine_bridge_tests PRIVATE openutau_audio_bridge)
add_dependencies(engine_bridge_tests fault_engine_host)
add_test(NAME engine_bridge_tests COMMAND engine_bridge_tests
  "dotnet"
  "${CMAKE_SOURCE_DIR}/../.build/dotnet/bin/OpenUtau.Vst.Engine.Host/Release/net8.0/OpenUtau.Vst.Engine.Host.dll"
  "$<TARGET_FILE:fault_engine_host>")

juce_add_plugin(OpenUtauVst
  COMPANY_NAME "OpenUtau"
  COMPANY_WEBSITE "https://www.openutau.com"
  COMPANY_EMAIL ""
  BUNDLE_ID "org.openutau.vst"
  PLUGIN_MANUFACTURER_CODE OpUt
  PLUGIN_CODE OuV3
  FORMATS VST3 Standalone
  PRODUCT_NAME "OpenUtau DAW"
  PLUGIN_NAME "OpenUtau DAW"
  DESCRIPTION "OpenUtau singing synthesis workstation"
  IS_SYNTH TRUE
  NEEDS_MIDI_INPUT FALSE
  NEEDS_MIDI_OUTPUT FALSE
  IS_MIDI_EFFECT FALSE
  EDITOR_WANTS_KEYBOARD_FOCUS TRUE
  COPY_PLUGIN_AFTER_BUILD FALSE
)

target_sources(OpenUtauVst PRIVATE
  src/plugin_editor.cpp
  src/plugin_processor.cpp
)
if(APPLE)
  enable_language(OBJCXX)
  target_sources(OpenUtauVst PRIVATE
    src/managed_editor_host_mac.cpp
    src/mac_space_key_forwarder.mm
  )
elseif(WIN32)
  target_sources(OpenUtauVst PRIVATE src/managed_editor_host_windows.cpp)
endif()
target_include_directories(OpenUtauVst PRIVATE include)
target_link_libraries(OpenUtauVst PRIVATE
  openutau_audio_bridge
  juce::juce_audio_utils
  juce::juce_gui_extra
  juce::juce_dsp
  juce::juce_recommended_config_flags
  juce::juce_recommended_lto_flags
  juce::juce_recommended_warning_flags
)
target_compile_definitions(OpenUtauVst PUBLIC
  JUCE_DISPLAY_SPLASH_SCREEN=0
  JUCE_REPORT_APP_USAGE=0
  JUCE_USE_CURL=0
  JUCE_WEB_BROWSER=0
  JUCE_VST3_CAN_REPLACE_VST2=0
)

juce_add_console_app(plugin_smoke_tests PRODUCT_NAME "OpenUtau DAW smoke host")
target_sources(plugin_smoke_tests PRIVATE tests/plugin_smoke_tests.cpp)
target_link_libraries(plugin_smoke_tests PRIVATE
  juce::juce_audio_processors
  juce::juce_recommended_config_flags
)
target_compile_definitions(plugin_smoke_tests PRIVATE
  JUCE_PLUGINHOST_VST3=1
  JUCE_USE_CURL=0
  JUCE_WEB_BROWSER=0
)
add_dependencies(plugin_smoke_tests OpenUtauVst_VST3)
add_test(NAME plugin_smoke_tests
  COMMAND plugin_smoke_tests "$<TARGET_FILE:OpenUtauVst_VST3>")

set(SINGING_FIXTURE_DIR "${CMAKE_BINARY_DIR}/singing-fixture")
add_test(NAME generate_singing_fixture
  COMMAND dotnet run --project
    "${CMAKE_SOURCE_DIR}/../bridge/OpenUtau.Vst.TestFixture"
    --configuration Release --no-build -- "${SINGING_FIXTURE_DIR}")
set_tests_properties(generate_singing_fixture PROPERTIES
  FIXTURES_SETUP singing_fixture)

juce_add_console_app(plugin_singing_e2e PRODUCT_NAME "OpenUtau audible VST3 E2E host")
target_sources(plugin_singing_e2e PRIVATE tests/plugin_singing_e2e.cpp)
target_link_libraries(plugin_singing_e2e PRIVATE
  openutau_audio_bridge
  juce::juce_audio_processors
  juce::juce_recommended_config_flags
)
target_compile_definitions(plugin_singing_e2e PRIVATE
  JUCE_PLUGINHOST_VST3=1
  JUCE_USE_CURL=0
  JUCE_WEB_BROWSER=0
)
add_dependencies(plugin_singing_e2e OpenUtauVst_VST3)
add_test(NAME plugin_singing_e2e
  COMMAND plugin_singing_e2e "$<TARGET_FILE:OpenUtauVst_VST3>"
    "${SINGING_FIXTURE_DIR}/project.ustx"
    "${SINGING_FIXTURE_DIR}/project-alternate.ustx")
set_tests_properties(plugin_singing_e2e PROPERTIES
  FIXTURES_REQUIRED singing_fixture
  TIMEOUT 180
  ENVIRONMENT
    "OPENUTAU_VST_DOTNET=dotnet;OPENUTAU_VST_ENGINE_HOST=${CMAKE_SOURCE_DIR}/../.build/dotnet/bin/OpenUtau.Vst.Engine.Host/Release/net8.0/OpenUtau.Vst.Engine.Host.dll;XDG_DATA_HOME=${SINGING_FIXTURE_DIR}/data;XDG_CACHE_HOME=${SINGING_FIXTURE_DIR}/cache;OPENUTAU_VST_DATA_HOME=${SINGING_FIXTURE_DIR}/data/OpenUtau;OPENUTAU_VST_CACHE_HOME=${SINGING_FIXTURE_DIR}/cache/OpenUtau")
