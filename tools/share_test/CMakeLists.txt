cmake_minimum_required(VERSION 3.21)

# Deliberately its own tiny project, and deliberately x86.
#
# The question this answers -- can this machine sustain a full-resolution
# Texture2D shared between an x86 and an x64 D3D12 device -- is the one that
# decides whether the DLSS architecture in DLSS_X64_HELPER_HANDOFF.md is
# possible here at all. Earlier attempts at cross-process GPU resource sharing
# in this project caused repeated NVIDIA device removals and a system hang
# (see MODDING.md), so it is tested in isolation: no game, no d3d8.dll, no
# RenderWare. If this fails, the failure is the sharing itself.
#
# Configure with the x86 toolchain, exactly like the shim:
#   cmake -S tools/share_test -B build-x86-share-test -G Ninja
#   cmake --build build-x86-share-test
project(Dx8to12ShareTest LANGUAGES CXX)

add_executable(dx8to12_share_test main.cpp)
target_compile_features(dx8to12_share_test PRIVATE cxx_std_20)
target_compile_definitions(dx8to12_share_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(dx8to12_share_test PRIVATE DXGI.lib D3D12.lib)

if(MSVC)
  target_compile_options(dx8to12_share_test PRIVATE /W4 /wd4100)
endif()
