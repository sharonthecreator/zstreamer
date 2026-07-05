# Copyright (c) 2026 sharonthecreator
# SPDX-License-Identifier: Apache-2.0

zephyr_library()

zephyr_include_directories(${ZEPHYR_ZSTREAMER_MODULE_DIR}/include)

zephyr_library_sources_ifdef(CONFIG_ZSTREAMER_I2S_SRC i2s_src.c)
