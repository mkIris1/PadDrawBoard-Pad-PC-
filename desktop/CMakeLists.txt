set(PDB_DESKTOP_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
set(PDB_PROTOCOL_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../protocol/cpp")

function(pdb_target_defaults target)
  target_compile_features(${target} PUBLIC cxx_std_20)
  target_compile_definitions(${target} PRIVATE NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX /permissive-)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endfunction()

add_library(paddraw_protocol STATIC
  "${CMAKE_CURRENT_SOURCE_DIR}/../protocol/cpp/paddraw_protocol.cpp"
)
target_include_directories(paddraw_protocol PUBLIC "${PDB_PROTOCOL_INCLUDE_DIR}")
pdb_target_defaults(paddraw_protocol)

add_library(pdb_input STATIC
  src/input/input_system.cpp
  src/input/mapper.cpp
  src/input/palm_guard.cpp
  src/input/profiles.cpp
  src/input/synthetic_injector.cpp
)
target_include_directories(pdb_input PUBLIC "${PDB_DESKTOP_INCLUDE_DIR}")
target_link_libraries(pdb_input PUBLIC user32)
pdb_target_defaults(pdb_input)

add_library(pdb_video STATIC
  src/video/backpressure_policy.cpp
  src/video/d3d11_nv12_converter.cpp
  src/video/desktop_duplication_capture.cpp
  src/video/h264_encoder.cpp
  src/video/monitor_enumerator.cpp
  src/video/resolution_ladder.cpp
  src/video/video_pipeline.cpp
)
target_include_directories(pdb_video PUBLIC "${PDB_DESKTOP_INCLUDE_DIR}")
target_link_libraries(pdb_video PUBLIC d3d11 dxgi mfplat mf mfuuid strmiids ole32 oleaut32)
pdb_target_defaults(pdb_video)

add_library(pdb_adb STATIC
  src/adb/backoff.cpp
  src/adb/client.cpp
  src/adb/input_probe.cpp
  src/adb/loopback.cpp
  src/adb/process.cpp
  src/adb/session.cpp
)
target_include_directories(pdb_adb PUBLIC "${PDB_DESKTOP_INCLUDE_DIR}")
target_link_libraries(pdb_adb PUBLIC ws2_32)
pdb_target_defaults(pdb_adb)

add_library(pdb_app STATIC
  src/app/config.cpp
  src/app/desktop_server.cpp
  src/app/framed_stream.cpp
  src/app/input_adapter.cpp
)
target_include_directories(pdb_app PUBLIC "${PDB_DESKTOP_INCLUDE_DIR}" "${PDB_PROTOCOL_INCLUDE_DIR}")
target_link_libraries(pdb_app PUBLIC paddraw_protocol pdb_input pdb_video pdb_adb shell32)
pdb_target_defaults(pdb_app)

add_executable(PadDrawBoard WIN32
  src/app/tray_main.cpp
  resources/paddrawboard.rc
)
target_include_directories(PadDrawBoard PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/resources")
target_link_libraries(PadDrawBoard PRIVATE pdb_app)
pdb_target_defaults(PadDrawBoard)
set_target_properties(PadDrawBoard PROPERTIES OUTPUT_NAME "PadDrawBoard")
if(MINGW)
  # This toolchain defaults to the GUI CRT startup even for ordinary test
  # executables. Keep the tray target Unicode and make each native test a
  # console process with its standard main entry point.
  target_link_options(PadDrawBoard PRIVATE -municode)
endif()

if(PDB_BUILD_TESTS)
  # Run manually on a logged-in Windows GPU host, never on a headless runner.
  add_executable(pdb_video_memory_probe tests/video/video_memory_probe.cpp)
  target_link_libraries(pdb_video_memory_probe PRIVATE pdb_video psapi)
  pdb_target_defaults(pdb_video_memory_probe)
  if(MINGW)
    target_link_options(pdb_video_memory_probe PRIVATE -mconsole)
  endif()
  function(pdb_add_native_test target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE ${ARGN})
    pdb_target_defaults(${target})
    # Keep test assertions active in Release builds.  This is scoped to test
    # executables so production targets retain the normal NDEBUG behavior.
    if(MSVC)
      target_compile_options(${target} PRIVATE /UNDEBUG)
    else()
      target_compile_options(${target} PRIVATE -UNDEBUG)
    endif()
    if(MINGW)
      target_link_options(${target} PRIVATE -mconsole)
    endif()
    add_test(NAME ${target} COMMAND ${target})
  endfunction()

  pdb_add_native_test(pdb_adb_tests tests/adb/adb_tests.cpp pdb_adb)
  pdb_add_native_test(pdb_input_logic_tests tests/input/input_logic_tests.cpp pdb_input)
  target_compile_definitions(pdb_input_logic_tests PRIVATE PDB_INPUT_TEST_MAIN)
  pdb_add_native_test(pdb_profile_resolver_tests tests/input/profile_resolver_tests.cpp pdb_input)
  target_compile_definitions(pdb_profile_resolver_tests PRIVATE PDB_PROFILE_TEST_MAIN)
  pdb_add_native_test(pdb_video_policy_tests tests/video/video_policy_tests.cpp pdb_video)
  target_compile_definitions(pdb_video_policy_tests PRIVATE PDB_VIDEO_TEST_MAIN)
  pdb_add_native_test(pdb_app_framing_tests tests/app/framed_stream_tests.cpp pdb_app)
  pdb_add_native_test(pdb_app_input_adapter_tests tests/app/input_adapter_tests.cpp pdb_app)
  pdb_add_native_test(pdb_app_config_tests tests/app/config_tests.cpp pdb_app)
endif()
