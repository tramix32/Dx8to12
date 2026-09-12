# Resolves DX8TO12_STREAMLINE_DIR and DX8TO12_NGX_DIR for every target that
# consumes NVIDIA's SDKs.
#
# A copy placed in third_party/ by hand wins. That keeps an offline machine
# building, and it is how a non-public SDK build gets used at all. When there
# is no such copy, the pinned public release is downloaded into the build tree
# at configure time instead.
#
# Nothing reaches the repository either way. third_party/ and build-*/ are both
# gitignored, and that is deliberate: these SDKs are NVIDIA-licensed and this
# fork is public, so they are fetched by whoever builds and never committed.
# This is the same arrangement the project already uses for
# D3D12MemoryAllocator, extended to the two SDKs that used to be copied in by
# hand.
#
# Streamline's binaries -- sl.interposer.lib, sl.*.dll, nvngx_dlss.dll -- are
# not in its git tree (not since SL 2.7.32); they ship in the release zip, so
# that is what is fetched. The NGX SDK keeps its headers and import libraries
# in the tree, so a shallow clone of the tag suffices there.

include_guard(GLOBAL)

set(DX8TO12_STREAMLINE_VERSION "2.12.0" CACHE STRING
    "Streamline SDK release downloaded when third_party/streamline is absent")
set(DX8TO12_NGX_VERSION "v310.7.0" CACHE STRING
    "NVIDIA DLSS (NGX) SDK tag downloaded when third_party/ngx is absent")
option(DX8TO12_FETCH_VENDOR_SDKS
       "Download the pinned Streamline and NGX SDKs at configure time when no local copy exists"
       ON)

get_filename_component(_dx8to12_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(DX8TO12_STREAMLINE_DIR "${_dx8to12_root}/third_party/streamline")
set(DX8TO12_NGX_DIR "${_dx8to12_root}/third_party/ngx")

# Both archives carry a CMakeLists.txt of their own that builds the SDK from
# source. That is not wanted: only the headers and the prebuilt binaries are.
# Pointing SOURCE_SUBDIR at a directory that does not exist is the documented
# way to have FetchContent_MakeAvailable populate without add_subdirectory.
set(_dx8to12_populate_only SOURCE_SUBDIR "cmake-must-not-build-this")

if(NOT EXISTS "${DX8TO12_STREAMLINE_DIR}/include/sl.h" AND DX8TO12_FETCH_VENDOR_SDKS)
  include(FetchContent)
  message(STATUS "Streamline SDK: no local copy in third_party/streamline; "
                 "fetching release v${DX8TO12_STREAMLINE_VERSION}")
  FetchContent_Declare(dx8to12_streamline_sdk
    URL "https://github.com/NVIDIA-RTX/Streamline/releases/download/v${DX8TO12_STREAMLINE_VERSION}/streamline-sdk-v${DX8TO12_STREAMLINE_VERSION}.zip"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    ${_dx8to12_populate_only})
  FetchContent_MakeAvailable(dx8to12_streamline_sdk)
  set(DX8TO12_STREAMLINE_DIR "${dx8to12_streamline_sdk_SOURCE_DIR}")
endif()

if(NOT EXISTS "${DX8TO12_NGX_DIR}/include/nvsdk_ngx.h" AND DX8TO12_FETCH_VENDOR_SDKS)
  include(FetchContent)
  message(STATUS "NGX SDK: no local copy in third_party/ngx; "
                 "fetching ${DX8TO12_NGX_VERSION}")
  FetchContent_Declare(dx8to12_ngx_sdk
    GIT_REPOSITORY https://github.com/NVIDIA/DLSS.git
    GIT_TAG "${DX8TO12_NGX_VERSION}"
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
    ${_dx8to12_populate_only})
  FetchContent_MakeAvailable(dx8to12_ngx_sdk)
  set(DX8TO12_NGX_DIR "${dx8to12_ngx_sdk_SOURCE_DIR}")
endif()

if(EXISTS "${DX8TO12_STREAMLINE_DIR}/include/sl.h")
  message(STATUS "Streamline SDK: ${DX8TO12_STREAMLINE_DIR}")
else()
  message(STATUS "Streamline SDK: not available (set DX8TO12_FETCH_VENDOR_SDKS=ON or place it in third_party/streamline)")
endif()
if(EXISTS "${DX8TO12_NGX_DIR}/include/nvsdk_ngx.h")
  message(STATUS "NGX SDK: ${DX8TO12_NGX_DIR}")
else()
  message(STATUS "NGX SDK: not available -- DLSS 5 Neural Rendering disabled")
endif()
