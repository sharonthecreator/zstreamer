/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared test helpers for zstreamer driver tests.
 */

#ifndef ZSTREAMER_TEST_HELPERS_H_
#define ZSTREAMER_TEST_HELPERS_H_

#include <stdint.h>

/**
 * Generate a deterministic byte at a given position so we can verify
 * the output without keeping the entire input in memory.
 */
static inline uint8_t pattern_byte(uint32_t pos) {
  return (uint8_t)((pos * 131u + 17u) & 0xffu);
}

#endif /* ZSTREAMER_TEST_HELPERS_H_ */
