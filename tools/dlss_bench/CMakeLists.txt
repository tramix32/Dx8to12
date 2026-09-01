cmake_minimum_required(VERSION 3.21)

# Measures what a DLSS evaluate actually costs on this GPU at this resolution,
# with nothing else in the picture: no game, no IPC, no shared textures.
#
# The transport cost is already known (tools/share_test: a full 2560x1440
# cross-process round trip sustains 338/s). The remaining unknown in the frame
# budget is DLSS itself, so it is isolated here rather than measured through
# two processes at once.
#
# x64 ONLY -- Streamline 2.12.0 ships no 32-bit binaries at all, which is the
# whole reason DLSS has to live in the helper process rather than in d3d8.dll.
#
#   cmake -S tools/dlss_bench -B build-x64-dlss-bench -G Ninja
#   cmake --build build-x64-dlss-bench
project(Dx8to12DlssBench LANGUAGES CXX)

set(DX8TO12_STREAMLINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/streamline")
if(NOT EXISTS "${DX8TO12_STREAMLINE_DIR}/include/sl.h")
  message(FATAL_ERROR
          "Streamline SDK not found at ${DX8TO12_STREAMLINE_DIR}. "
          "This tool exists only to measure DLSS, so unlike the rest of the "
          "project it cannot degrade gracefully without it.")
endif()

add_executable(dx8to12_dlss_bench main.cpp)
target_compile_features(dx8to12_dlss_bench PRIVATE cxx_std_20)
target_compile_definitions(dx8to12_dlss_bench PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(dx8to12_dlss_bench PRIVATE "${DX8TO12_STREAMLINE_DIR}/include")

# Unlike rt_helper, this executable DOES link the interposer. That is safe
# precisely because it is a separate program: routing its D3D12 entry points
# through Streamline cannot affect the RT helper or the game.
#
# D3D12.lib and DXGI.lib are deliberately NOT linked. sl.interposer.lib
# exports D3D12CreateDevice and CreateDXGIFactory* itself, and that is the
# entire point -- Streamline has to wrap the device at creation time. Linking
# the real import libraries first resolves those symbols to the untouched
# system entry points, the device is never wrapped, and every DLSS call then
# fails with eErrorFeatureNotSupported (observed exactly that way here).
target_link_libraries(dx8to12_dlss_bench PRIVATE
  "${DX8TO12_STREAMLINE_DIR}/lib/x64/sl.interposer.lib")

if(MSVC)
  target_compile_options(dx8to12_dlss_bench PRIVATE /W4 /wd4100)
endif()

foreach(sl_dll sl.interposer.dll sl.common.dll sl.dlss.dll nvngx_dlss.dll)
  if(EXISTS "${DX8TO12_STREAMLINE_DIR}/bin/x64/${sl_dll}")
    add_custom_command(TARGET dx8to12_dlss_bench POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${DX8TO12_STREAMLINE_DIR}/bin/x64/${sl_dll}"
              "$<TARGET_FILE_DIR:dx8to12_dlss_bench>/${sl_dll}")
  endif()
endforeach()
