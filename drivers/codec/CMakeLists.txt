# Copyright (c) 2026 sharonthecreator
# SPDX-License-Identifier: Apache-2.0

zephyr_library()

zephyr_include_directories(${ZEPHYR_ZSTREAMER_MODULE_DIR}/include)

if(CONFIG_ZSTREAMER_OPUS)
  # The opus west project (west.yml) has no Zephyr module glue, so it
  # is located via the workspace topdir and compiled into this library.
  set(OPUS_DIR ${WEST_TOPDIR}/modules/lib/opus)
  if(NOT EXISTS ${OPUS_DIR}/opus_sources.mk)
    message(FATAL_ERROR
      "libopus not found at ${OPUS_DIR} — run 'west update opus'")
  endif()

  include(${OPUS_DIR}/cmake/OpusFunctions.cmake)
  get_opus_sources(SILK_SOURCES ${OPUS_DIR}/silk_sources.mk silk_sources)
  get_opus_sources(SILK_SOURCES_FIXED ${OPUS_DIR}/silk_sources.mk
                   silk_sources_fixed)
  get_opus_sources(CELT_SOURCES ${OPUS_DIR}/celt_sources.mk celt_sources)
  get_opus_sources(OPUS_SOURCES ${OPUS_DIR}/opus_sources.mk opus_sources)

  set(opus_all_sources
      ${silk_sources} ${silk_sources_fixed} ${celt_sources} ${opus_sources})
  list(TRANSFORM opus_all_sources PREPEND ${OPUS_DIR}/)

  zephyr_library_sources(${opus_all_sources} opus_common.c)
  zephyr_library_include_directories(
    ${OPUS_DIR}/include
    ${OPUS_DIR}/celt
    ${OPUS_DIR}/silk
    ${OPUS_DIR}/silk/fixed
    ${OPUS_DIR})
  zephyr_library_compile_definitions(
    OPUS_BUILD
    FIXED_POINT=1
    DISABLE_FLOAT_API
    NONTHREADSAFE_PSEUDOSTACK)
  # Third-party code: silence its warnings, they are not ours to fix.
  set_source_files_properties(${opus_all_sources} PROPERTIES
                              COMPILE_OPTIONS "-w")
endif()

zephyr_library_sources_ifdef(CONFIG_ZSTREAMER_OPUS_ENCODE_NODE
                             opus_encode_node.c)
zephyr_library_sources_ifdef(CONFIG_ZSTREAMER_OPUS_DECODE_NODE
                             opus_decode_node.c)
