# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.28.0)

# The board, SoC, drivers and DT bindings for mido live in this repository,
# next to the application. Point the build at it so a plain
# `cmake -B build -DBOARD=mido` works, without -DBOARD_ROOT/-DSOC_ROOT/-DDTS_ROOT.
get_filename_component(MIDO_ROOT ${CMAKE_CURRENT_LIST_DIR}/.. ABSOLUTE)
list(APPEND BOARD_ROOT ${MIDO_ROOT})
list(APPEND SOC_ROOT ${MIDO_ROOT})
list(APPEND DTS_ROOT ${MIDO_ROOT})

# The drivers and Kconfig for this board are a Zephyr module in this same
# repository; register it so they are built and their options are visible.
list(APPEND ZEPHYR_EXTRA_MODULES ${MIDO_ROOT})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(zephyr_mido)

target_sources(app PRIVATE src/main.c)

# Host-side render check: fake MDP5 registers, real driver, real LVGL.
if(CONFIG_ARCH_POSIX)
  target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../tests/sim/mdp5_sim.c)
  target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../tests/sim)
endif()
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
