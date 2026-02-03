# Copyright (c) 2026 zstreamer contributors
# SPDX-License-Identifier: Apache-2.0

zephyr_library()

zephyr_include_directories(${ZEPHYR_ZSTREAMER_MODULE_DIR}/include)

zephyr_library_sources(
  zstreamer_graph.c
  zstreamer_node.c
)
