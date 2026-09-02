cmake_minimum_required(VERSION 3.21)

# Separate from rt_helper on purpose. Streamline requires slInit to run before
# the D3D12 device is created so its interposer can wrap it, which means
# linking sl.interposer.lib -- and that routes every D3D12 entry point in the
# process through Streamline. The RT helper must not be routed through it, so
# these are two binaries rather than two modes of one.
#
# Must be x64: Streamline/NGX ships x64-only, which is the whole reason a
# helper process exists at all.
#   cmake -S dlaa_helper -B build-x64-dlaa-helper -G Ninja
#   cmake --build build-x64-dlaa-helper
project(Dx8to12DlaaHelper LANGUAGES CXX)

add_executable(dx8to12_dlaa_helper dlaa_helper_main.cpp)
target_compile_features(dx8to12_dlaa_helper PRIVATE cxx_std_20)
target_compile_definitions(dx8to12_dlaa_helper PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(dx8to12_dlaa_helper
                           PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../shared")

set(DX8TO12_STREAMLINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/streamline")
if(EXISTS "${DX8TO12_STREAMLINE_DIR}/include/sl.h")
  message(STATUS "Streamline SDK found: ${DX8TO12_STREAMLINE_DIR}")
  target_include_directories(dx8to12_dlaa_helper
                             PRIVATE "${DX8TO12_STREAMLINE_DIR}/include")
  target_compile_definitions(dx8to12_dlaa_helper PRIVATE DX8TO12_HAVE_STREAMLINE)
  # sl.interposer.lib *instead of* D3D12.lib/DXGI.lib, never alongside them:
  # linking both makes the loader resolve D3D12CreateDevice to the real one and
  # Streamline never sees the device. This was one of four bring-up traps found
  # in tools/dlss_bench.
  target_link_libraries(dx8to12_dlaa_helper
                        PRIVATE "${DX8TO12_STREAMLINE_DIR}/lib/x64/sl.interposer.lib")
  foreach(sl_dll sl.interposer.dll sl.common.dll sl.dlss.dll nvngx_dlss.dll)
    if(EXISTS "${DX8TO12_STREAMLINE_DIR}/bin/x64/${sl_dll}")
      add_custom_command(TARGET dx8to12_dlaa_helper POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${DX8TO12_STREAMLINE_DIR}/bin/x64/${sl_dll}"
                "$<TARGET_FILE_DIR:dx8to12_dlaa_helper>/${sl_dll}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/dist"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${DX8TO12_STREAMLINE_DIR}/bin/x64/${sl_dll}"
                "${CMAKE_BINARY_DIR}/dist/${sl_dll}")
    else()
      message(WARNING "Streamline SDK is missing bin/x64/${sl_dll}")
    endif()
  endforeach()
else()
  # Absence must not break the build: without the SDK the helper still builds
  # and still works, in loopback only, and says so.
  message(STATUS "Streamline SDK not found -- DLAA helper builds in loopback-only mode")
  target_link_libraries(dx8to12_dlaa_helper PRIVATE DXGI.lib D3D12.lib)
endif()

# Optional NGX SDK (github.com/NVIDIA/DLSS), needed only for DLSS 5 Neural
# Rendering. Streamline 2.12 has no feature for it -- its kFeature list stops
# at DLSS, DLSS_G, DLSS_RR and DirectSR -- so NR has to be driven through NGX
# directly, which means this SDK in addition to Streamline rather than instead
# of it. Supplied by the developer and never committed, exactly like
# Streamline; absence must not break anything.
#
# The runtime it needs, nvngx_dlssnr.dll, is a separate matter again: NVIDIA
# ships DLSS 5 for RTX 50 only, so running it on RTX 40 needs a build made for
# that. Put whichever you have next to the helper.
set(DX8TO12_NGX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/ngx")
if(EXISTS "${DX8TO12_NGX_DIR}/include/nvsdk_ngx.h")
  message(STATUS "NGX SDK found: ${DX8TO12_NGX_DIR}")
  target_include_directories(dx8to12_dlaa_helper PRIVATE "${DX8TO12_NGX_DIR}/include")
  target_compile_definitions(dx8to12_dlaa_helper PRIVATE DX8TO12_HAVE_NGX)
  foreach(ngx_lib nvsdk_ngx_d_dbg.lib nvsdk_ngx_d.lib)
    if(EXISTS "${DX8TO12_NGX_DIR}/lib/x64/${ngx_lib}")
      target_link_libraries(dx8to12_dlaa_helper PRIVATE "${DX8TO12_NGX_DIR}/lib/x64/${ngx_lib}")
      break()
    endif()
  endforeach()
else()
  message(STATUS "NGX SDK not found at ${DX8TO12_NGX_DIR} -- DLSS 5 Neural Rendering disabled")
endif()

# RegGetValueW, for the driver's own NGX directory. Linked in both branches
# because the Streamline one replaces D3D12.lib/DXGI.lib, not the rest of Win32.
target_link_libraries(dx8to12_dlaa_helper PRIVATE Advapi32.lib)

if(MSVC)
  target_compile_options(dx8to12_dlaa_helper PRIVATE /W4 /wd4100 /wd4505)
endif()

add_custom_command(
  TARGET dx8to12_dlaa_helper
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/dist"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:dx8to12_dlaa_helper>"
          "${CMAKE_BINARY_DIR}/dist/"
  COMMENT "Copying x64 DLAA helper to dist/"
  VERBATIM)
