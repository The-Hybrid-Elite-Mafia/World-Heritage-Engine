cmake_minimum_required(VERSION 3.25)
project(WorldHeritageEngine LANGUAGES CXX)

# Static MSVC runtime: the .exe files run on any Windows 10/11 PC without installing
# the Visual C++ Redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(WHE_BUILD_TESTS "Build the test suite" ON)

add_library(whe_core STATIC
  src/mapped_file.cpp
  src/audio_decode.cpp
  src/flac.cpp
  src/sample_store.cpp
  src/sample_library.cpp
  src/dfd_stream.cpp
  src/voice.cpp
  src/hybrid.cpp
  src/instrument.cpp
  src/engines.cpp
  src/instrument_manager.cpp
  src/simd/simd_dispatch.cpp
  src/simd/kernels_scalar.cpp
  src/simd/kernels_avx2.cpp
  src/simd/kernels_avx512.cpp
)
target_include_directories(whe_core PUBLIC include)

if(MSVC)
  target_compile_options(whe_core PRIVATE /W4 /permissive- /O2)
  set_source_files_properties(src/simd/kernels_avx2.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
  set_source_files_properties(src/simd/kernels_avx512.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX512")
else()
  target_compile_options(whe_core PRIVATE -Wall -Wextra -O2)
  set_source_files_properties(src/simd/kernels_avx2.cpp PROPERTIES COMPILE_OPTIONS "-mavx2;-mfma")
  set_source_files_properties(src/simd/kernels_avx512.cpp PROPERTIES COMPILE_OPTIONS "-mavx512f;-mfma")
endif()

find_package(Threads REQUIRED)
target_link_libraries(whe_core PUBLIC Threads::Threads)

if(WIN32)
  target_compile_definitions(whe_core PUBLIC _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
  target_link_libraries(whe_core PUBLIC avrt ole32 winmm)
  # Real-time host: WASAPI output + Windows MIDI input (+ ASIO when the SDK is present)
  add_executable(whe_play tools/whe_play.cpp src/platform/wasapi_output.cpp)
  target_link_libraries(whe_play PRIVATE whe_core)

  # Steinberg ASIO SDK (GPLv3 or Steinberg proprietary). Never committed:
  # tools/fetch-asio-sdk.ps1 downloads it from Steinberg into external/asiosdk/sdk.
  set(WHE_ASIO_SDK_DIR "${PROJECT_SOURCE_DIR}/external/asiosdk/sdk" CACHE PATH "Steinberg ASIO SDK root (contains common/asio.h)")
  if(EXISTS "${WHE_ASIO_SDK_DIR}/common/asio.h" AND EXISTS "${WHE_ASIO_SDK_DIR}/host/pc/asiolist.cpp")
    add_library(whe_asio_sdk STATIC
      "${WHE_ASIO_SDK_DIR}/common/asio.cpp"
      "${WHE_ASIO_SDK_DIR}/host/asiodrivers.cpp"
      "${WHE_ASIO_SDK_DIR}/host/pc/asiolist.cpp")
    target_include_directories(whe_asio_sdk SYSTEM PUBLIC
      "${WHE_ASIO_SDK_DIR}/common" "${WHE_ASIO_SDK_DIR}/host" "${WHE_ASIO_SDK_DIR}/host/pc")
    # Legacy Steinberg sources: ANSI registry calls, pre-C++11 idioms — keep them away from our strict flags.
    set_target_properties(whe_asio_sdk PROPERTIES CXX_STANDARD 14)
    if(MSVC)
      target_compile_options(whe_asio_sdk PRIVATE /permissive /W0)
    else()
      target_compile_options(whe_asio_sdk PRIVATE -w -fpermissive)
    endif()
    target_compile_definitions(whe_asio_sdk PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
    target_link_libraries(whe_asio_sdk PUBLIC ole32 advapi32 user32 uuid)
    target_sources(whe_play PRIVATE src/platform/asio_output.cpp)
    target_compile_definitions(whe_play PRIVATE WHE_HAS_ASIO=1)
    target_link_libraries(whe_play PRIVATE whe_asio_sdk)
    message(STATUS "WHE: ASIO SDK found at ${WHE_ASIO_SDK_DIR} — whe_play gets ASIO output")
  else()
    message(STATUS "WHE: ASIO SDK not found — whe_play is WASAPI-only (run tools/fetch-asio-sdk.ps1)")
  endif()
endif()

add_executable(whe_ingest tools/whe_ingest.cpp)
target_link_libraries(whe_ingest PRIVATE whe_core)

add_executable(whe_render tools/whe_render.cpp)
target_link_libraries(whe_render PRIVATE whe_core)

if(WHE_BUILD_TESTS)
  add_executable(whe_tests tests/test_main.cpp)
  target_link_libraries(whe_tests PRIVATE whe_core)
endif()
