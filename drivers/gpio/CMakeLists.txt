# Copyright (c) 2026 sharonthecreator
# SPDX-License-Identifier: Apache-2.0

zephyr_library()

zephyr_include_directories(${ZEPHYR_ZSTREAMER_MODULE_DIR}/include)

zephyr_library_sources_ifdef(CONFIG_ZSTREAMER_GPIO_PULSE_NODE gpio_pulse_node.c)
zephyr_library_sources_ifdef(CONFIG_ZSTREAMER_GPIO_BUTTON_SRC button_src.c)
