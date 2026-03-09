# Copyright (c) 2026 sharonthecreator
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# Pick up test-local DTS bindings (fake DAC).
list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(zstreamer_dac_test)

target_sources(app PRIVATE
  src/main.c
  src/fake_dac.c
)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
