cmake_minimum_required(VERSION 3.21)

# This project is intentionally configured separately from the x86 D3D8 shim.
# A 32-bit process cannot observe native NVIDIA DXR, while this executable has
# to be x64.  Example:
#   cmake -S rt_helper -B build-x64-rt-helper -G Ninja
#   cmake --build build-x64-rt-helper
project(Dx8to12RtHelper LANGUAGES CXX)

add_executable(dx8to12_rt_helper rt_helper_main.cpp)
target_compile_features(dx8to12_rt_helper PRIVATE cxx_std_20)
target_compile_definitions(dx8to12_rt_helper PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(dx8to12_rt_helper PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../shared")
target_link_libraries(dx8to12_rt_helper PRIVATE DXGI.lib D3D12.lib dxcompiler.lib)

# DXR libraries are compiled at runtime by the x64 helper.  Keep the SDK DXC
# runtime next to the helper so the game never depends on a system installation.
set(DX8TO12_DXC_REDIST "C:/Program Files (x86)/Windows Kits/10/Redist/D3D/x64/dxcompiler.dll")
set(DX8TO12_DXIL_REDIST "C:/Program Files (x86)/Windows Kits/10/Redist/D3D/x64/dxil.dll")
if(EXISTS "${DX8TO12_DXC_REDIST}")
  add_custom_command(TARGET dx8to12_rt_helper POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DX8TO12_DXC_REDIST}"
            "$<TARGET_FILE_DIR:dx8to12_rt_helper>/dxcompiler.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DX8TO12_DXC_REDIST}"
            "${CMAKE_BINARY_DIR}/dist/dxcompiler.dll")
endif()
if(EXISTS "${DX8TO12_DXIL_REDIST}")
  add_custom_command(TARGET dx8to12_rt_helper POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DX8TO12_DXIL_REDIST}"
            "$<TARGET_FILE_DIR:dx8to12_rt_helper>/dxil.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DX8TO12_DXIL_REDIST}"
            "${CMAKE_BINARY_DIR}/dist/dxil.dll")
endif()

if(MSVC)
  target_compile_options(dx8to12_rt_helper PRIVATE /W4 /wd4100 /wd4505)
endif()

# Optional NVIDIA Streamline SDK (DLSS/DLAA). Supplied by the developer, never
# committed -- it has its own licence and redistribution terms, and this is a
# public repo (see .gitignore). Absence must not break anything: without it the
# helper still builds and the game still runs, --dlss-probe just says so.
#
# Note what is deliberately NOT done here: sl.interposer.lib is not linked.
# Linking it routes the whole process's D3D12 entry points through Streamline,
# which would change the existing RT path for every session even when DLSS is
# not in use. rt_helper_main.cpp loads sl.interposer.dll dynamically instead,
# only inside --dlss-probe.
set(DX8TO12_STREAMLINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/streamline")
if(EXISTS "${DX8TO12_STREAMLINE_DIR}/include/sl.h")
  message(STATUS "Streamline SDK found: ${DX8TO12_STREAMLINE_DIR}")
  target_include_directories(dx8to12_rt_helper PRIVATE "${DX8TO12_STREAMLINE_DIR}/include")
  target_compile_definitions(dx8to12_rt_helper PRIVATE DX8TO12_HAVE_STREAMLINE)
  # Only the DLLs the probe (and later, DLSS itself) actually needs at runtime.
  # nvngx_dlss.dll is the DLSS model; sl.common carries the shared plumbing.
  foreach(sl_dll sl.interposer.dll sl.common.dll sl.dlss.dll nvngx_dlss.dll)
    if(EXISTS "${DX8TO12_STREAMLINE_DIR}/bin/x64/${sl_dll}")
      add_custom_command(TARGET dx8to12_rt_helper POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${DX8TO12_STREAMLINE_DIR}/bin/x64/${sl_dll}"
                "$<TARGET_FILE_DIR:dx8to12_rt_helper>/${sl_dll}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${DX8TO12_STREAMLINE_DIR}/bin/x64/${sl_dll}"
                "${CMAKE_BINARY_DIR}/dist/${sl_dll}")
    else()
      message(WARNING "Streamline SDK is missing bin/x64/${sl_dll}")
    endif()
  endforeach()
else()
  message(STATUS "Streamline SDK not found at ${DX8TO12_STREAMLINE_DIR} -- DLSS probing disabled")
endif()

add_custom_command(
  TARGET dx8to12_rt_helper
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/dist"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:dx8to12_rt_helper>"
          "${CMAKE_BINARY_DIR}/dist/"
  COMMENT "Copying x64 DXR helper to dist/"
  VERBATIM)
