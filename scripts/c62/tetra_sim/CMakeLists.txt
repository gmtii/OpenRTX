# SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Host simulator of the C62 TETRA receive chain, see tetra_ul_sim.c.
#
#   cmake -S OpenRTX/scripts/c62/tetra_sim -B build_sim \
#         -DLIBTETRA=modules/lib/libtetra
#   cmake --build build_sim && build_sim/tetra_ul_sim

cmake_minimum_required(VERSION 3.16)
project(tetra_sim LANGUAGES C)

set(LIBTETRA "" CACHE PATH "libtetra source tree")
if(NOT EXISTS ${LIBTETRA}/CMakeLists.txt)
  message(FATAL_ERROR "Set LIBTETRA to the libtetra source tree")
endif()

# As on the radio, but with the uplink decoder
set(TETRA_BUILD_TOOLS OFF)
set(TETRA_BUILD_TESTS OFF)
set(TETRA_ENABLE_CRYPTO OFF)
set(TETRA_UPLINK_DECODE ON)
add_subdirectory(${LIBTETRA} libtetra EXCLUDE_FROM_ALL)

set(OPENRTX_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
add_executable(tetra_ul_sim
  tetra_ul_sim.c
  ${OPENRTX_ROOT}/openrtx/src/protocols/TETRA/TetraDemod.c)
target_include_directories(tetra_ul_sim PRIVATE ${OPENRTX_ROOT}/openrtx/include)
target_compile_options(tetra_ul_sim PRIVATE -O2 -Wall -Wextra)
target_link_libraries(tetra_ul_sim PRIVATE tetra m)
