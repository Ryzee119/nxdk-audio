cmake_minimum_required(VERSION 3.25)
project(nxdk-audio C)

# 1. Define common strict warnings for all languages
set(COMMON_WARNINGS "-Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wconversion -Wsign-conversion -Wcast-align -Wdouble-promotion")

include(ExternalProject)
ExternalProject_Add(
    a56_tool
    GIT_REPOSITORY https://github.com/XboxDev/a56.git
    UPDATE_COMMAND ""
    CONFIGURE_COMMAND ""
    BUILD_IN_SOURCE 1
    BUILD_COMMAND make -j1 "CC=cc -std=gnu89 -w" "HOSTCC=cc -std=gnu89 -w"
    INSTALL_COMMAND ""
)

ExternalProject_Get_Property(a56_tool SOURCE_DIR)
set(A56_EXE ${SOURCE_DIR}/a56)

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/passthrough.out
    COMMAND bash "-c" "${A56_EXE} -o ${CMAKE_CURRENT_SOURCE_DIR}/passthrough.out ${CMAKE_CURRENT_SOURCE_DIR}/passthrough.a56 | tail -n 2"
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/passthrough.a56
    COMMENT "Compiling DSP code: passthrough.a56 -> passthrough.out"
    VERBATIM
)

add_library(nxaudio STATIC audio_buffer.c audio_core.c audio_voice.c)
add_dependencies(nxaudio a56_tool)

separate_arguments(COMMON_WARNINGS)
target_compile_options(nxaudio PRIVATE ${COMMON_WARNINGS})

target_include_directories(nxaudio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
set_property(SOURCE audio_buffer.c audio_core.c audio_voice.c APPEND PROPERTY OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/passthrough.out")

option(NXAUDIO_BUILD_EXAMPLES "Build example applications" ON)

if(NXAUDIO_BUILD_EXAMPLES)
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/example/cmake")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)

    add_executable(spatial_test example/spatial_test.c)
    target_include_directories(spatial_test PRIVATE ${SDL2_INCLUDE_DIRS} ${NXDK_DIR}/lib/pbkit)
    target_compile_options(spatial_test PRIVATE ${SDL2_CFLAGS_OTHER})
    target_compile_definitions(spatial_test PRIVATE __XBOX__ -DSDL_DISABLE_JOYSTICK_INIT_DELAY)
    target_link_libraries(spatial_test PRIVATE nxaudio ${SDL2_LIBRARIES})
    target_link_libraries(spatial_test PRIVATE ${NXDK_DIR}/lib/libnxdk_automount_d.lib ${NXDK_DIR}/lib/libpbkit.lib)
    target_link_options(spatial_test PRIVATE "-include:_automount_d_drive")

    add_executable(midi_player example/midi_player.c)
    target_include_directories(midi_player PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${SDL2_INCLUDE_DIRS} ${NXDK_DIR}/lib/pbkit)
    target_compile_options(midi_player PRIVATE ${SDL2_CFLAGS_OTHER})
    target_compile_definitions(midi_player PRIVATE __XBOX__ -DSDL_DISABLE_JOYSTICK_INIT_DELAY)
    target_link_libraries(midi_player PRIVATE nxaudio ${SDL2_LIBRARIES})
    target_link_libraries(midi_player PRIVATE ${NXDK_DIR}/lib/libnxdk_automount_d.lib)
    target_link_options(midi_player PRIVATE "-include:_automount_d_drive")

    add_executable(stream_test example/stream_test.c)
    target_include_directories(stream_test PRIVATE ${SDL2_INCLUDE_DIRS})
    target_compile_options(stream_test PRIVATE ${SDL2_CFLAGS_OTHER})
    target_compile_definitions(stream_test PRIVATE __XBOX__ -DSDL_DISABLE_JOYSTICK_INIT_DELAY)
    target_link_libraries(stream_test PRIVATE nxaudio ${SDL2_LIBRARIES})
    target_link_libraries(stream_test PRIVATE ${NXDK_DIR}/lib/libnxdk_automount_d.lib)
    target_link_options(stream_test PRIVATE "-include:_automount_d_drive")

    set(EXAMPLES spatial_test midi_player stream_test)

    # Post build steps to copy assets, generate XBE, and XISO
    include (Postbuild)
endif()
