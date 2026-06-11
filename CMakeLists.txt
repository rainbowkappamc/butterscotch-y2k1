cmake_minimum_required(VERSION 3.21)
project(butterscotch C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra)

set(PLATFORM "" CACHE STRING "Platform backend")

# Build metadata
set(BUTTERSCOTCH_COMMIT_HASH "unknown" CACHE STRING "Git commit hash")
set(BUTTERSCOTCH_COMMIT_DATE "unknown" CACHE STRING "Git commit date")
add_compile_definitions(
    BUTTERSCOTCH_COMMIT_HASH="${BUTTERSCOTCH_COMMIT_HASH}"
    BUTTERSCOTCH_COMMIT_DATE="${BUTTERSCOTCH_COMMIT_DATE}"
)

if(PLATFORM STREQUAL "")
    message(FATAL_ERROR "PLATFORM is not set! Use -DPLATFORM=glfw (or another supported platform).")
endif()

option(ENABLE_BC16 "Enable support for Bytecode Version 16" ON)
option(ENABLE_BC17 "Enable support for Bytecode Version 17" ON)
option(ENABLE_LEGACY_GL "Enable the legacy OpenGL renderer" ON)
option(ENABLE_MODERN_GL "Enable the modern OpenGL renderer" ON)

if(NOT ENABLE_BC16 AND NOT ENABLE_BC17)
    message(FATAL_ERROR "You need to build Butterscotch with at least one bytecode version enabled!")
endif()
if(ENABLE_BC16)
    add_compile_definitions(ENABLE_BC16)
endif()
if(ENABLE_BC17)
    add_compile_definitions(ENABLE_BC17)
endif()
if(ENABLE_LEGACY_GL)
    add_compile_definitions(ENABLE_LEGACY_GL)
endif()
if(ENABLE_MODERN_GL)
    add_compile_definitions(ENABLE_MODERN_GL)
endif()

# ── Shared Butterscotch engine sources ────────────────────────────────────────
file(GLOB SOURCES src/butterscotch/*.c)

# ── Platform sources ──────────────────────────────────────────────────────────
file(GLOB PLATFORM_SOURCES src/platform/${PLATFORM}/*.c)

add_executable(butterscotch ${SOURCES} ${PLATFORM_SOURCES})

# Core include dirs: butterscotch shared + platform-specific
target_include_directories(butterscotch PRIVATE
    ${CMAKE_SOURCE_DIR}/src/butterscotch
    ${CMAKE_SOURCE_DIR}/src/platform/${PLATFORM}
    ${CMAKE_SOURCE_DIR}/src/platform
)

# ── stb_ds ────────────────────────────────────────────────────────────────────
target_include_directories(butterscotch PUBLIC vendor/stb/ds)

# ── MD5 ───────────────────────────────────────────────────────────────────────
target_sources(butterscotch PRIVATE vendor/md5/md5.c)
target_include_directories(butterscotch PUBLIC vendor/md5)

# ── GLFW platform ─────────────────────────────────────────────────────────────
if(PLATFORM STREQUAL "glfw")
    # stb_image + stb_vorbis
    target_include_directories(butterscotch PUBLIC vendor/stb/image)
    target_include_directories(butterscotch PUBLIC vendor/stb/vorbis)

    # miniaudio
    target_include_directories(butterscotch PUBLIC vendor/miniaudio)

    # GLAD
    option(ENABLE_GLES "Build against OpenGL ES 3.0 instead of desktop GL 4.1 Core" OFF)
    if(ENABLE_GLES)
        add_compile_definitions(ENABLE_GLES)
        add_library(glad STATIC vendor/glad-gles/src/glad.c)
        target_include_directories(glad PUBLIC vendor/glad-gles/include)
    else()
        add_library(glad STATIC vendor/glad/src/glad.c)
        target_include_directories(glad PUBLIC vendor/glad/include)
    endif()

    # VM profiler / tracing options
    option(ENABLE_VM_GML_PROFILER "Enable Butterscotch VM/interpreter profiler" ON)
    if(ENABLE_VM_GML_PROFILER)
        add_compile_definitions(ENABLE_VM_GML_PROFILER)
    endif()

    option(ENABLE_VM_TRACING "Enable Butterscotch VM/interpreter tracing checks" ON)
    if(ENABLE_VM_TRACING)
        add_compile_definitions(ENABLE_VM_TRACING)
    endif()

    option(ENABLE_VM_OPCODE_PROFILER "Enable Butterscotch VM opcode count profiler" ON)
    if(ENABLE_VM_OPCODE_PROFILER)
        add_compile_definitions(ENABLE_VM_OPCODE_PROFILER)
    endif()

    option(ENABLE_SPATIAL_GRID_LOGS "Enable Spatial Grid logs" OFF)
    if(ENABLE_SPATIAL_GRID_LOGS)
        add_compile_definitions(ENABLE_SPATIAL_GRID_LOGS)
    endif()

    option(ENABLE_VM_STUB_LOGS "Enable VM unknown/stubbed function logs" ON)
    if(ENABLE_VM_STUB_LOGS)
        add_compile_definitions(ENABLE_VM_STUB_LOGS)
    endif()

    add_compile_definitions(USE_MINIAUDIO)

    # debug_font (used by debugscreen)
    file(GLOB DEBUG_FONT_SOURCES src/butterscotch/debug_font/*.c)
    target_sources(butterscotch PRIVATE ${DEBUG_FONT_SOURCES})
    target_include_directories(butterscotch PRIVATE
        ${CMAKE_SOURCE_DIR}/src/butterscotch/debug_font)

    if(MINGW)
        find_package(glfw3 REQUIRED)
        set(GLFW3_LIBRARIES glfw3)
        target_compile_definitions(butterscotch PRIVATE
            _WIN32_WINNT=0x0601 NTDDI_VERSION=0x06010000 WIN32_LEAN_AND_MEAN)
        target_link_libraries(butterscotch
            ${GLFW3_LIBRARIES} glad m opengl32 gdi32 winmm bz2)
        target_link_options(butterscotch PRIVATE -static)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Haiku")
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(GLFW3 REQUIRED glfw3)
        target_include_directories(butterscotch PRIVATE ${GLFW3_INCLUDE_DIRS})
        target_link_directories(butterscotch PRIVATE ${GLFW3_LIBRARY_DIRS})
        target_link_libraries(butterscotch ${GLFW3_LIBRARIES} glad m pthread bz2)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(GLFW3 REQUIRED glfw3)
        target_include_directories(butterscotch PRIVATE ${GLFW3_INCLUDE_DIRS})
        target_link_directories(butterscotch PRIVATE ${GLFW3_LIBRARY_DIRS})
        target_link_libraries(butterscotch ${GLFW3_LIBRARIES} glad m pthread dl bz2)
    endif()

    # AddressSanitizer in Debug builds
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(ENABLE_ASAN_DEFAULT ON)
    else()
        set(ENABLE_ASAN_DEFAULT OFF)
    endif()
    set(ENABLE_ASAN ${ENABLE_ASAN_DEFAULT} CACHE BOOL "Enable AddressSanitizer")
    if(ENABLE_ASAN AND NOT MINGW AND NOT CMAKE_SYSTEM_NAME STREQUAL "Haiku")
        target_compile_options(butterscotch PRIVATE -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(butterscotch PRIVATE -fsanitize=address)
    endif()

    # gamecontrollerdb
    add_custom_command(TARGET butterscotch POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/vendor/gamecontrollerdb.txt
            $<TARGET_FILE_DIR:butterscotch>/gamecontrollerdb.txt
    )

elseif(PLATFORM STREQUAL "ps2")
    file(GLOB DEBUG_FONT_SOURCES src/butterscotch/debug_font/*.c)
    target_sources(butterscotch PRIVATE ${DEBUG_FONT_SOURCES})
    target_include_directories(butterscotch PRIVATE
        ${CMAKE_SOURCE_DIR}/src/butterscotch/debug_font)

    add_compile_definitions(USE_FLOAT_REALS)
    add_compile_definitions(NO_RVALUE_INT64)
    add_compile_definitions(PLATFORM_PS2)

    option(ENABLE_VM_GML_PROFILER "Enable Butterscotch VM/interpreter profiler" OFF)
    if(ENABLE_VM_GML_PROFILER)
        add_compile_definitions(ENABLE_VM_GML_PROFILER)
    endif()
    option(ENABLE_VM_TRACING "Enable Butterscotch VM/interpreter tracing checks" OFF)
    if(ENABLE_VM_TRACING)
        add_compile_definitions(ENABLE_VM_TRACING)
    endif()
    option(ENABLE_VM_OPCODE_PROFILER "Enable Butterscotch VM opcode count profiler" OFF)
    if(ENABLE_VM_OPCODE_PROFILER)
        add_compile_definitions(ENABLE_VM_OPCODE_PROFILER)
    endif()
    option(ENABLE_SPATIAL_GRID_LOGS "Enable Spatial Grid logs" OFF)
    if(ENABLE_SPATIAL_GRID_LOGS)
        add_compile_definitions(ENABLE_SPATIAL_GRID_LOGS)
    endif()
    option(ENABLE_VM_STUB_LOGS "Enable VM unknown/stubbed function logs" OFF)
    if(ENABLE_VM_STUB_LOGS)
        add_compile_definitions(ENABLE_VM_STUB_LOGS)
    endif()
    option(ENABLE_PS2_RENDERER_LOGS "Enable hot path logs in the PS2 renderer" OFF)
    if(ENABLE_PS2_RENDERER_LOGS)
        add_compile_definitions(ENABLE_PS2_RENDERER_LOGS)
    endif()

    file(GLOB PS2_AUDIO_SOURCES src/butterscotch/audio/ps2/*.c)
    target_sources(butterscotch PRIVATE ${PS2_AUDIO_SOURCES})
    target_include_directories(butterscotch PRIVATE
        ${CMAKE_SOURCE_DIR}/src/butterscotch/audio/ps2)
    add_compile_definitions(USE_PS2_AUDIO)

    target_compile_options(butterscotch PRIVATE
        -fsingle-precision-constant -Werror=double-promotion
        -fomit-frame-pointer -ffast-math)

    if(PS2)
        target_include_directories(butterscotch PRIVATE
            $ENV{PS2SDK}/ee/include $ENV{PS2SDK}/common/include
            $ENV{PS2DEV}/ee/include $ENV{GSKIT}/include)
        target_link_directories(butterscotch PRIVATE
            $ENV{PS2SDK}/ee/lib $ENV{GSKIT}/lib)
        target_link_libraries(butterscotch gskit dmakit audsrv pad mc kbd patches kernel c m)
    endif()

else()
    message(FATAL_ERROR "Unknown platform: ${PLATFORM}")
endif()
