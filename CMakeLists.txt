# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(svr2011 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

set(SVR2011_SOURCES
    src/main.cpp
    src/d3d_census.cpp
    src/native/geometry_cache.cpp
    src/native/native_renderer.cpp
    src/native/pipelines.cpp
    src/native/shader_library.cpp
    src/native/texture_layout.cpp
    src/native/textures.cpp
    src/native/vk_context.cpp
    src/native/write_watch.cpp
)

# Native renderer shaders: converted locally from the player's game by
# scripts/convert-shaders.ps1 (game-derived, never committed). Without them the
# native renderer stays off.
set(SVR2011_SHADER_CACHE "${CMAKE_CURRENT_SOURCE_DIR}/cache/shader-native/shader_cache.cpp")
if(EXISTS "${SVR2011_SHADER_CACHE}")
    list(APPEND SVR2011_SOURCES "${SVR2011_SHADER_CACHE}")
else()
    list(APPEND SVR2011_SOURCES src/native/shader_cache_empty.cpp)
endif()
set(SVR2011_FETCH_SLOTS "${CMAKE_CURRENT_SOURCE_DIR}/cache/shader-native/fetch_slots.cpp")
if(EXISTS "${SVR2011_FETCH_SLOTS}")
    list(APPEND SVR2011_SOURCES "${SVR2011_FETCH_SLOTS}")
else()
    list(APPEND SVR2011_SOURCES src/native/shader_fetch_slots_empty.cpp)
endif()

if(WIN32)
    add_executable(svr2011 WIN32 ${SVR2011_SOURCES})
else()
    add_executable(svr2011 ${SVR2011_SOURCES})
endif()

rexglue_setup_target(svr2011 GPU_PLUGINS xenos)

# The native renderer un-tiles textures with the SDK's Xenos texture layout
# code (BSD-3), which lives in the GPU plugin rather than the host runtime.
if(REXSDK_DIR)
    target_sources(svr2011 PRIVATE
        ${REXSDK_DIR}/src/graphics/pipeline/texture/util.cpp)
endif()
target_include_directories(svr2011 PRIVATE src src/native)

# zstd (decompression only) and smol-v unpack the converted shader cache; both
# come with XenosRecomp in .tools/xenosrecomp (BSD-3 and MIT).
set(SVR2011_XENOSRECOMP_THIRDPARTY "${CMAKE_CURRENT_SOURCE_DIR}/.tools/xenosrecomp/thirdparty")
set(SVR2011_ZSTD "${SVR2011_XENOSRECOMP_THIRDPARTY}/zstd/lib")
add_library(svr2011_native_deps STATIC
    ${SVR2011_ZSTD}/common/debug.c
    ${SVR2011_ZSTD}/common/entropy_common.c
    ${SVR2011_ZSTD}/common/error_private.c
    ${SVR2011_ZSTD}/common/fse_decompress.c
    ${SVR2011_ZSTD}/common/xxhash.c
    ${SVR2011_ZSTD}/common/zstd_common.c
    ${SVR2011_ZSTD}/decompress/huf_decompress.c
    ${SVR2011_ZSTD}/decompress/zstd_ddict.c
    ${SVR2011_ZSTD}/decompress/zstd_decompress.c
    ${SVR2011_ZSTD}/decompress/zstd_decompress_block.c
    ${SVR2011_XENOSRECOMP_THIRDPARTY}/smol-v/source/smolv.cpp)
target_compile_definitions(svr2011_native_deps PRIVATE ZSTD_DISABLE_ASM)
target_compile_options(svr2011_native_deps PRIVATE -w)
target_include_directories(svr2011_native_deps PUBLIC
    ${SVR2011_ZSTD} ${SVR2011_XENOSRECOMP_THIRDPARTY}/smol-v/source)
target_link_libraries(svr2011 PRIVATE svr2011_native_deps)

# The v0.10.0 source-tree runtime does not propagate ImGui's include directory
# to the host, although its public UI headers include imgui.h.
if(REXSDK_DIR)
    target_include_directories(svr2011 PRIVATE
        $<TARGET_PROPERTY:imgui::imgui,INTERFACE_INCLUDE_DIRECTORIES>)
    # The native renderer includes the SDK's Vulkan headers, which include RenderDoc's.
    target_include_directories(svr2011 PRIVATE ${REXSDK_DIR}/thirdparty/renderdoc
        ${REXSDK_DIR}/thirdparty/xxHash)
endif()

# Offline tests of the native renderer's pure code (decoding, tiling): Catch2
# from the SDK tree. scripts/build.ps1 -Action Test configures, builds and runs
# them; they never ship.
option(SVR_BUILD_TESTS "Build the native renderer's offline tests" OFF)
if(SVR_BUILD_TESTS AND REXSDK_DIR)
    enable_testing()
    add_executable(svr2011_native_tests
        tests/native/test_guest_decode.cpp
        tests/native/test_texture_layout.cpp
        tests/native/test_write_watch.cpp
        src/native/texture_layout.cpp
        src/native/write_watch.cpp
        ${REXSDK_DIR}/src/graphics/pipeline/texture/util.cpp
        ${REXSDK_DIR}/thirdparty/catch2/extras/catch_amalgamated.cpp)
    target_include_directories(svr2011_native_tests PRIVATE src
        ${REXSDK_DIR}/thirdparty/catch2/extras)
    target_link_libraries(svr2011_native_tests PRIVATE rex::runtime)
    add_test(NAME svr2011_native_tests COMMAND svr2011_native_tests)
endif()
