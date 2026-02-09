# Copyright (c) 2026 sharonthecreator
# SPDX-License-Identifier: Apache-2.0

if(CONFIG_ZSTREAMER)
  add_subdirectory(lib/zstreamer)
  add_subdirectory_ifdef(CONFIG_ZSTREAMER_NODE drivers/zstreamer)
endif()
