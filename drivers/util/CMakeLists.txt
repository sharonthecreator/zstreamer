# Copyright (c) 2026 sharonthecreator
# SPDX-License-Identifier: Apache-2.0

zephyr_library()

zephyr_include_directories(${ZEPHYR_ZSTREAMER_MODULE_DIR}/include)

zephyr_library_sources_ifdef(CONFIG_ZSTREAMER_UTIL_LOOPER_NODE looper_node.c)
zephyr_library_sources_ifdef(CONFIG_ZSTREAMER_UTIL_RANDOM_TIMER_SRC random_timer_src.c)
