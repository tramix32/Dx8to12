cmake_minimum_required(VERSION 3.11...3.26)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(DX8TO12_USE_ALLOCATOR "Use D3D12MemoryAllocator instead of manual suballocation" OFF)
option(DX8TO12_ENABLE_VALIDATION "Enable the D3D12 debug layer and TRACE_ENTRY logging (dev builds)" ON)
option(DX8TO12_ENABLE_MINDEBUG "Enable only targeted low-overhead diagnostics (release-mindebug builds)" OFF)
option(DX8TO12_ENABLE_D3D12_DEBUG_LAYER "Enable only the D3D12/DXGI debug layer without dev TRACE logging" OFF)
option(DX8TO12_FORCE_GPU_IDLE "Wait for the just-submitted GPU frame (diagnostic A/B)" OFF)
option(DX8TO12_PASSTHROUGH_OOB_INDICES "Submit out-of-bounds indexed draws without truncating them (compatibility A/B)" OFF)
option(DX8TO12_VALIDATE_DEVICE_ALWAYS_FAIL "Return D3DERR_INVALIDCALL from ValidateDevice like the D3D11 reference port (compatibility A/B)" OFF)
option(DX8TO12_BUFFER_SHADOW "Give every vertex/index buffer a full CPU shadow and upload dirty ranges through the command list, like the d3d8to11 reference port (missing-geometry A/B)" OFF)
option(DX8TO12_KEEP_TARGET_LOD "Keep the diagnosed near-road model permanently faded so its LOD is never culled (in-process probe; pollutes baseline measurements)" OFF)
# ON by default: this is a confirmed fix, not an experiment. GTA VC writes past
# the end of its own static index buffers (measured: 219 distinct buffers in one
# session) and then draws those indices; without the pad, DrawIndexedPrimitive
# clamps the count and silently drops the trailing triangles of a material --
# the "disappearing road/building" bug. Turn OFF only to reproduce that bug for
# an A/B. See MISSING_TEXTURES_DIAGNOSTIC_HANDOFF.md.
option(DX8TO12_PAD_BUFFERS "Over-allocate vertex/index buffers so the game's writes and draws past their declared end stay in valid memory instead of being clamped away" ON)
option(DX8TO12_DRAW_STATE_CACHE "Skip re-recording PSO/root/vertex-buffer bindings when nothing they depend on changed (faster; see kCacheDrawStateBindings in device_limits.h before enabling)" OFF)
option(DX8TO12_TEMPORAL_JITTER "Offset the projection by a per-frame sub-pixel Halton sequence (prerequisite for DLAA/DLSS; on its own it only makes the image shimmer)" OFF)
option(DX8TO12_SCENE_TARGET "Render the game's scene into an offscreen colour target and copy it to the backbuffer before mod callbacks (prerequisite for DLAA/DLSS; on its own the image must look identical)" OFF)
option(DX8TO12_MOTION_VECTORS "Reconstruct camera motion vectors from the depth buffer into an offscreen R16G16_FLOAT target (prerequisite for DLAA/DLSS; on its own nothing samples it -- pair with the debug view to see anything)" OFF)
option(DX8TO12_MOTION_VECTORS_DEBUG "Draw the reconstructed motion vectors over the scene as false colour. Implies DX8TO12_MOTION_VECTORS; makes the game unplayable but is the only way to verify the vectors by eye" OFF)

if(DX8TO12_MOTION_VECTORS_DEBUG AND NOT DX8TO12_MOTION_VECTORS)
  message(STATUS "DX8TO12_MOTION_VECTORS_DEBUG implies DX8TO12_MOTION_VECTORS; enabling it")
  set(DX8TO12_MOTION_VECTORS ON)
endif()
if(DX8TO12_MOTION_VECTORS AND NOT DX8TO12_SCENE_TARGET)
  # The pass reads the scene's depth buffer and the debug view draws onto the
  # scene colour target. Both are the scene target's, not the backbuffer's.
  message(FATAL_ERROR
          "DX8TO12_MOTION_VECTORS requires DX8TO12_SCENE_TARGET=ON")
endif()

if(DX8TO12_ENABLE_VALIDATION AND DX8TO12_ENABLE_MINDEBUG)
  message(FATAL_ERROR
          "DX8TO12_ENABLE_MINDEBUG requires DX8TO12_ENABLE_VALIDATION=OFF")
endif()

project(
  Dx8to12
  VERSION 0.1
  LANGUAGES CXX)

add_library(d3d8 SHARED)
set_property(TARGET d3d8 PROPERTY CXX_STANDARD 20)
set_property(TARGET d3d8 PROPERTY CXX_STANDARD_REQUIRED ON)

add_subdirectory(src)

target_link_libraries(d3d8 PUBLIC DXGI.lib D3D12.lib D3DCompiler.lib dxguid.lib)
target_include_directories(d3d8 PUBLIC src src/DirectX8 third_party shared)
target_compile_definitions(
  d3d8 PRIVATE WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS NOMINMAX
               CURRENT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
if(DX8TO12_USE_ALLOCATOR)
  target_compile_definitions(d3d8
                             PRIVATE USE_ALLOCATOR="${DX8TO12_USE_ALLOCATOR}")
endif()
if(DX8TO12_ENABLE_VALIDATION)
  target_compile_definitions(d3d8 PRIVATE DX8TO12_ENABLE_VALIDATION)
endif()
if(DX8TO12_ENABLE_MINDEBUG)
  target_compile_definitions(d3d8 PRIVATE DX8TO12_ENABLE_MINDEBUG)
endif()
if(DX8TO12_ENABLE_D3D12_DEBUG_LAYER)
  target_compile_definitions(d3d8 PRIVATE DX8TO12_ENABLE_D3D12_DEBUG_LAYER)
endif()
if(DX8TO12_FORCE_GPU_IDLE)
  target_compile_definitions(d3d8 PRIVATE DX8TO12_FORCE_GPU_IDLE)
endif()
if(DX8TO12_PASSTHROUGH_OOB_INDICES)
  target_compile_definitions(d3d8 PRIVATE DX8TO12_PASSTHROUGH_OOB_INDICES)
endif()
if(DX8TO12_VALIDATE_DEVICE_ALWAYS_FAIL)
  target_compile_definitions(d3d8 PRIVATE DX8TO12_VALIDATE_DEVICE_ALWAYS_FAIL)
endif()
if(DX8TO12_BUFFER_SHADOW)
  target_compile_definitions(d3d8 PRIVATE DX8TO12_BUFFER_SHADOW)
endif()
if(DX8TO12_KEEP_TARGET_LOD)
  target_compile_definitions(d3d8 PRIVATE DX8TO12_KEEP_TARGET_LOD)
endif()
if(DX8TO12_PAD_BUFFERS)
  target_compile_definitions(d3d8 PRIVATE DX8TO12_PAD_BUFFERS)
endif()
if(DX8TO12_DRAW_STATE_CACHE)
  target_compile_definitions(d3d8 PRIVATE DX8TO12_DRAW_STATE_CACHE)
endif()
if(DX8TO12_TEMPORAL_JITTER)
  target_compile_definitions(d3d8 PRIVATE DX8TO12_TEMPORAL_JITTER)
endif()
if(DX8TO12_SCENE_TARGET)
  target_compile_definitions(d3d8 PRIVATE DX8TO12_SCENE_TARGET)
endif()
if(DX8TO12_MOTION_VECTORS)
  target_compile_definitions(d3d8 PRIVATE DX8TO12_MOTION_VECTORS)
endif()
if(DX8TO12_MOTION_VECTORS_DEBUG)
  target_compile_definitions(d3d8 PRIVATE DX8TO12_MOTION_VECTORS_DEBUG)
endif()

# Dev builds retain the normal AixLog sink and crash diagnostics. Both release
# profiles compile ordinary LOG(...) statements away completely: the clean
# release has no file logging at all, while release-mindebug writes only its
# explicitly selected compact diagnostics through a dedicated path.
if(NOT DX8TO12_ENABLE_VALIDATION)
  target_compile_definitions(d3d8 PRIVATE DX8TO12_DISABLE_LOGGING)
endif()

# target_compile_options(d3d8 PRIVATE "$<$<CONFIG:Debug>:/Ox>")

target_precompile_headers(d3d8 PRIVATE src/pch.h)

if(MSVC)
  target_compile_options(d3d8 PRIVATE /W4 /wd4100 /wd4505)
  target_compile_options(d3d8 PRIVATE /MP)
  target_link_options(d3d8 PRIVATE /LARGEADDRESSAWARE)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_compile_options(d3d8 PRIVATE -Wno-unused-parameter)
  target_compile_options(
    d3d8
    PRIVATE -Wall
            -Wno-c++98-compat
            -Wno-reserved-macro-identifier
            -Wno-reserved-identifier
            -Wno-gnu-anonymous-struct
            -Wno-c++98-compat-pedantic
            -Wno-nested-anon-types
            -Wno-extra-semi
            -Wno-newline-eof
            -Wno-signed-enum-bitfield
            -Wno-language-extension-token
            -Wno-switch-enum
            -Wno-undefined-reinterpret-cast
            -Wno-old-style-cast
            # Maybe someday I'll fix sign conversion.
            -Wno-sign-conversion
            -Werror)
endif()

# target_compile_options(d3d8 PRIVATE /fsanitize=address)

# Copy just the files that need to be dropped into a game install (the DLL
# and its matching PDB, nothing else from the build tree) into build/dist/
# after every build.
add_custom_command(
  TARGET d3d8
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory
          "${CMAKE_BINARY_DIR}/dist"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:d3d8>"
          "${CMAKE_BINARY_DIR}/dist/"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_PDB_FILE:d3d8>"
          "${CMAKE_BINARY_DIR}/dist/"
  COMMENT "Copying d3d8.dll (+ pdb) to ${CMAKE_BINARY_DIR}/dist"
  VERBATIM)

if(DX8TO12_USE_ALLOCATOR)
  include(FetchContent)
  include(CMakePrintHelpers)

  FetchContent_Declare(
    D3D12MemoryAllocator
    GIT_REPOSITORY
      https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator
    GIT_TAG 554cd184af2a85dfe10eba8c30bdca28e22dd12a)
  FetchContent_MakeAvailable(D3D12MemoryAllocator)
  get_target_property(D3D12MemoryAllocator_INCLUDES D3D12MemoryAllocator
                      INCLUDE_DIRECTORIES)

  target_link_libraries(d3d8 PUBLIC D3D12MemoryAllocator)
  target_include_directories(d3d8 PRIVATE "${D3D12MemoryAllocator_INCLUDES}")
endif()

