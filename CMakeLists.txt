cmake_minimum_required(VERSION 3.25)
set(XBE_TITLE nxdk-audio)
set(XBOX_ISO_DIR ${CMAKE_CURRENT_BINARY_DIR}/xiso)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

project(${XBE_TITLE} C)

# 1. Define common strict warnings for all languages
set(COMMON_WARNINGS "-Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wconversion -Wsign-conversion -Wcast-align -Wdouble-promotion")

# 2. Define C++ specific warnings
set(CXX_SPECIFIC_WARNINGS "-Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual")

# 3. Apply to Debug flags
set(CMAKE_ASM_FLAGS_DEBUG "${CMAKE_ASM_FLAGS_DEBUG} -g -gdwarf-4 ${COMMON_WARNINGS}")
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -g -gdwarf-4 ${COMMON_WARNINGS}")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -gdwarf-4 ${COMMON_WARNINGS} ${CXX_SPECIFIC_WARNINGS}")

# 4. Linker flags (unchanged)
set(CMAKE_EXE_LINKER_FLAGS_DEBUG "${CMAKE_EXE_LINKER_FLAGS_DEBUG} -debug")

set(CMAKE_ASM_FLAGS_RELEASE "${CMAKE_ASM_FLAGS_RELEASE} -O2")
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O2")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O2")

add_executable(${XBE_TITLE} main.c audio.c)

target_link_libraries(${XBE_TITLE} PUBLIC ${NXDK_DIR}/lib/libnxdk_automount_d.lib)
target_link_options(${XBE_TITLE} PRIVATE "-include:_automount_d_drive")

set_property(SOURCE audio.c APPEND PROPERTY OBJECT_DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/passthrough.out")

# Post build steps to copy assets, generate XBE, and XISO
include (Postbuild)
