/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared test helpers for zstreamer driver tests.
 */

#ifndef ZSTREAMER_TEST_HELPERS_H_
#define ZSTREAMER_TEST_HELPERS_H_

#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>

#include <zstreamer/graph.h>

/**
 * Generate a deterministic byte at a given position so we can verify
 * the output without keeping the entire input in memory.
 */
static inline uint8_t pattern_byte(uint32_t pos) {
  return (uint8_t)((pos * 131u + 17u) & 0xffu);
}

/**
 * Assert that every buffer in the graph pool has been freed (ref count 0).
 * Sleeps briefly to let downstream threads finish processing after a stop.
 * Requires CONFIG_NET_BUF_POOL_USAGE=y.
 */
static inline void assert_pool_free(const struct device *graph) {
  const struct zstreamer_graph_config *cfg = graph->config;
  struct net_buf_pool *pool = cfg->pool;

  /* Wait for the pipeline to end the processing. */
  k_msleep(50);
  zassert_equal(atomic_get(&pool->avail_count), pool->buf_count,
                "pool leak: avail %ld != total %u",
                atomic_get(&pool->avail_count), pool->buf_count);
}

#endif /* ZSTREAMER_TEST_HELPERS_H_ */
