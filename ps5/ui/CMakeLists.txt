# The app's UI on a PC: the console's screens in a window or as a PNG, with sample data.
# (The console build compiles the same src/ files with its own main; see ps5/app.)
cmake_minimum_required(VERSION 3.20)
project(stremio_preview C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

set(third_party "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

# GLFW opens the preview's window. A local checkout is used when one is named, otherwise
# the release is fetched.
set(STREMIO_GLFW_DIR "" CACHE PATH "A GLFW source tree to build instead of fetching one")
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
if(STREMIO_GLFW_DIR AND EXISTS "${STREMIO_GLFW_DIR}/CMakeLists.txt")
    add_subdirectory("${STREMIO_GLFW_DIR}" glfw EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(glfw
        URL https://github.com/glfw/glfw/releases/download/3.4/glfw-3.4.zip
        URL_HASH SHA256=b5ec004b2712fd08e8861dc271428f048775200a2df719ccf575143e06e2dce9)
    FetchContent_MakeAvailable(glfw)
endif()

# The WebP decoder: Stremio's poster service sends WebP.
file(GLOB webp_sources
    "${third_party}/libwebp/src/dec/*.c"
    "${third_party}/libwebp/src/dsp/*.c"
    "${third_party}/libwebp/src/utils/*.c")
list(FILTER webp_sources EXCLUDE REGEX "/dsp/(enc|cost|ssim|picture)[^/]*\\.c$|_enc[^/]*\\.c$")
add_library(webpdecode STATIC ${webp_sources})
target_include_directories(webpdecode PUBLIC "${third_party}/libwebp/src" PRIVATE "${third_party}/libwebp")
# libwebp picks its SSE4.1 routines at run time but compiles them unconditionally on x86.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    target_compile_options(webpdecode PRIVATE -msse4.1)
endif()

add_library(stremio_ui STATIC
    src/account_data.cpp
    src/app.cpp
    src/board_data.cpp
    src/details_data.cpp
    src/details_screen.cpp
    src/draw_util.cpp
    src/icons.cpp
    src/images.cpp
    "${third_party}/nanovg/nanovg.c")
target_include_directories(stremio_ui PUBLIC src "${third_party}/nanovg" "${third_party}")
target_link_libraries(stremio_ui PUBLIC webpdecode)

add_executable(stremio_preview host/preview_main.cpp "${third_party}/glad/glad.c")
target_include_directories(stremio_preview PRIVATE "${third_party}/stb")
target_link_libraries(stremio_preview PRIVATE stremio_ui glfw)
if(WIN32)
    target_compile_definitions(stremio_preview PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(stremio_ui PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(webpdecode PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
