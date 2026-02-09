# Copyright (c) 2026 sharonthecreator
# SPDX-License-Identifier: Apache-2.0

zephyr_library()

zephyr_include_directories(${ZEPHYR_ZSTREAMER_MODULE_DIR}/include)

zephyr_library_sources_ifdef(CONFIG_ZSTREAMER_NODE_TEST_NUMGEN_SRC src_numgen.c)
zephyr_library_sources_ifdef(CONFIG_ZSTREAMER_NODE_TEST_FAKESINK sink_fake.c)
