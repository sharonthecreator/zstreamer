/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 *
 * DAC sink zstreamer node driver tests.
 *
 * Pipeline: numgen-src → dac-sinker (12-bit) + dac-sinker-8bit (8-bit)
 * Both sinks receive every buffer from numgen; each writes to its own
 * fake DAC so recordings don't interleave.
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/net_buf.h>
#include <zephyr/ztest.h>

#include <zstreamer/node.h>
#include <zstreamer/source.h>
#include <zstreamer_test/helpers.h>

#include "fake_dac.h"

/* ------------------------------------------------------------------ */
/* DTS nodes                                                           */
/* ------------------------------------------------------------------ */

#define GRAPH_NODE          DT_NODELABEL(dac_streaming_graph)
#define NUMGEN_NODE         DT_NODELABEL(numgen)
#define DAC_SINK_NODE       DT_NODELABEL(dac_sinker)
#define DAC_SINK_8BIT_NODE  DT_NODELABEL(dac_sinker_8bit)
#define FAKE_DAC_NODE       DT_NODELABEL(fake_dac)
#define FAKE_DAC_8BIT_NODE  DT_NODELABEL(fake_dac_8bit)

static const struct device *graph_dev        = DEVICE_DT_GET(GRAPH_NODE);
static const struct device *numgen_dev       = DEVICE_DT_GET(NUMGEN_NODE);
static const struct device *dac_sink_dev     = DEVICE_DT_GET(DAC_SINK_NODE);
static const struct device *dac_sink_8bit_dev = DEVICE_DT_GET(DAC_SINK_8BIT_NODE);
static const struct device *fake_dac_dev     = DEVICE_DT_GET(FAKE_DAC_NODE);
static const struct device *fake_dac_8bit_dev = DEVICE_DT_GET(FAKE_DAC_8BIT_NODE);

/* ------------------------------------------------------------------ */
/* Fixture                                                             */
/* ------------------------------------------------------------------ */

static void cleanup(void *fixture) {
  ARG_UNUSED(fixture);

  zstreamer_source_stop(numgen_dev);
  k_msleep(50);
  fake_dac_reset(fake_dac_dev);
  fake_dac_reset(fake_dac_8bit_dev);
}

ZTEST_SUITE(zstreamer_dac, NULL, NULL, cleanup, cleanup, NULL);

/* ------------------------------------------------------------------ */
/* Basic tests                                                         */
/* ------------------------------------------------------------------ */

ZTEST(zstreamer_dac, test_devices_ready) {
  zassert_true(device_is_ready(graph_dev), "graph not ready");
  zassert_true(device_is_ready(numgen_dev), "numgen not ready");
  zassert_true(device_is_ready(dac_sink_dev), "dac_sink not ready");
  zassert_true(device_is_ready(dac_sink_8bit_dev), "dac_sink_8bit not ready");
  zassert_true(device_is_ready(fake_dac_dev), "fake_dac not ready");
  zassert_true(device_is_ready(fake_dac_8bit_dev), "fake_dac_8bit not ready");
}

ZTEST(zstreamer_dac, test_start_stop) {
  int ret;

  ret = zstreamer_source_start(numgen_dev);
  zassert_equal(ret, 0, "start failed: %d", ret);

  ret = zstreamer_source_start(numgen_dev);
  zassert_equal(ret, -EALREADY, "double start: %d", ret);

  ret = zstreamer_source_stop(numgen_dev);
  zassert_equal(ret, 0, "stop failed: %d", ret);

  ret = zstreamer_source_stop(numgen_dev);
  zassert_equal(ret, -EALREADY, "double stop: %d", ret);
}

/* ------------------------------------------------------------------ */
/* 12-bit DAC output tests                                             */
/* ------------------------------------------------------------------ */

ZTEST(zstreamer_dac, test_16bit_dac_called) {
  int ret;

  ret = zstreamer_source_start(numgen_dev);
  zassert_equal(ret, 0);

  k_msleep(200);

  ret = zstreamer_source_stop(numgen_dev);
  zassert_equal(ret, 0);

  k_msleep(50);

  zassert_true(fake_dac_get_write_count(fake_dac_dev) > 0,
               "DAC write_value was never called");
}

ZTEST(zstreamer_dac, test_16bit_values_in_range) {
  int ret;

  ret = zstreamer_source_start(numgen_dev);
  zassert_equal(ret, 0);

  k_msleep(200);

  ret = zstreamer_source_stop(numgen_dev);
  zassert_equal(ret, 0);

  k_msleep(50);

  uint32_t n = fake_dac_get_recorded_count(fake_dac_dev);

  zassert_true(n > 0, "no values recorded");

  for (uint32_t i = 0; i < n; i++) {
    uint32_t val = fake_dac_get_recorded_value(fake_dac_dev, i);

    zassert_true(val <= 4095, "sample %u: value %u out of 12-bit range", i,
                 val);
  }
}

/**
 * Verify the 12-bit DAC output matches the numgen byte pattern.
 *
 * numgen fills buffers with incrementing bytes.  The 16-bit dac_sink
 * interprets consecutive byte-pairs as little-endian uint16_t and
 * masks to 12 bits.
 */
ZTEST(zstreamer_dac, test_16bit_data_integrity) {
  int ret;

  ret = zstreamer_source_start(numgen_dev);
  zassert_equal(ret, 0);

  k_msleep(50);

  ret = zstreamer_source_stop(numgen_dev);
  zassert_equal(ret, 0);

  k_msleep(50);

  uint32_t n = fake_dac_get_recorded_count(fake_dac_dev);

  zassert_true(n >= 2, "need at least 2 DAC values, got %u", n);

  uint32_t first = fake_dac_get_recorded_value(fake_dac_dev, 0);
  uint32_t second = fake_dac_get_recorded_value(fake_dac_dev, 1);

  /* Derive byte offset from the first two samples. */
  int start = -1;

  for (int b = 0; b < 256; b += 2) {
    uint8_t lo0 = (uint8_t)(b & 0xFF);
    uint8_t hi0 = (uint8_t)((b + 1) & 0xFF);
    uint32_t exp0 = ((uint16_t)hi0 << 8 | lo0) & 0xFFF;

    uint8_t lo1 = (uint8_t)((b + 2) & 0xFF);
    uint8_t hi1 = (uint8_t)((b + 3) & 0xFF);
    uint32_t exp1 = ((uint16_t)hi1 << 8 | lo1) & 0xFFF;

    if (exp0 == first && exp1 == second) {
      start = b;
      break;
    }
  }

  zassert_true(start >= 0,
               "could not derive byte offset from first=%u second=%u", first,
               second);

  for (uint32_t i = 0; i < n; i++) {
    uint32_t b = (start + 2 * i) & 0xFFFF;
    uint8_t lo = (uint8_t)(b & 0xFF);
    uint8_t hi = (uint8_t)((b + 1) & 0xFF);
    uint32_t expected = ((uint16_t)hi << 8 | lo) & 0xFFF;
    uint32_t actual = fake_dac_get_recorded_value(fake_dac_dev, i);

    zassert_equal(actual, expected,
                  "sample %u: DAC %u != expected %u (lo=%u hi=%u)", i, actual,
                  expected, lo, hi);
  }
}

/* ------------------------------------------------------------------ */
/* 8-bit DAC output tests                                              */
/* ------------------------------------------------------------------ */

ZTEST(zstreamer_dac, test_8bit_dac_called) {
  int ret;

  ret = zstreamer_source_start(numgen_dev);
  zassert_equal(ret, 0);

  k_msleep(200);

  ret = zstreamer_source_stop(numgen_dev);
  zassert_equal(ret, 0);

  k_msleep(50);

  zassert_true(fake_dac_get_write_count(fake_dac_8bit_dev) > 0,
               "8-bit DAC write_value was never called");
}

ZTEST(zstreamer_dac, test_8bit_values_in_range) {
  int ret;

  ret = zstreamer_source_start(numgen_dev);
  zassert_equal(ret, 0);

  k_msleep(200);

  ret = zstreamer_source_stop(numgen_dev);
  zassert_equal(ret, 0);

  k_msleep(50);

  uint32_t n = fake_dac_get_recorded_count(fake_dac_8bit_dev);

  zassert_true(n > 0, "no 8-bit values recorded");

  for (uint32_t i = 0; i < n; i++) {
    uint32_t val = fake_dac_get_recorded_value(fake_dac_8bit_dev, i);

    zassert_true(val <= 255, "sample %u: value %u out of 8-bit range", i, val);
  }
}

/**
 * Verify the 8-bit DAC output matches the numgen byte pattern.
 *
 * The 8-bit dac_sink reads one byte at a time from the buffer and
 * masks to 8 bits.  numgen produces incrementing bytes, so the DAC
 * values should be a contiguous ascending sequence (mod 256).
 */
ZTEST(zstreamer_dac, test_8bit_data_integrity) {
  int ret;

  ret = zstreamer_source_start(numgen_dev);
  zassert_equal(ret, 0);

  k_msleep(50);

  ret = zstreamer_source_stop(numgen_dev);
  zassert_equal(ret, 0);

  k_msleep(50);

  uint32_t n = fake_dac_get_recorded_count(fake_dac_8bit_dev);

  zassert_true(n >= 2, "need at least 2 values, got %u", n);

  /* Derive starting byte from first value, verify sequence. */
  uint32_t first = fake_dac_get_recorded_value(fake_dac_8bit_dev, 0);

  for (uint32_t i = 0; i < n; i++) {
    uint32_t expected = (first + i) & 0xFF;
    uint32_t actual = fake_dac_get_recorded_value(fake_dac_8bit_dev, i);

    zassert_equal(actual, expected, "sample %u: DAC %u != expected %u", i,
                  actual, expected);
  }
}

ZTEST(zstreamer_dac, test_8bit_more_samples_than_16bit) {
  int ret;

  ret = zstreamer_source_start(numgen_dev);
  zassert_equal(ret, 0);

  k_msleep(200);

  ret = zstreamer_source_stop(numgen_dev);
  zassert_equal(ret, 0);

  k_msleep(50);

  uint32_t w_16 = fake_dac_get_write_count(fake_dac_dev);
  uint32_t w_8 = fake_dac_get_write_count(fake_dac_8bit_dev);

  zassert_true(w_16 > 0, "12-bit: no samples");
  zassert_true(w_8 > 0, "8-bit: no samples");

  /*
   * Both sinks receive the same buffers.  The 8-bit sink processes
   * 1 byte per sample, the 16-bit sink processes 2 bytes per sample.
   * So the 8-bit sink should produce ~2x more DAC writes.
   */
  zassert_true(w_8 > w_16,
               "8-bit writes (%u) should exceed 16-bit writes (%u)", w_8,
               w_16);
}

/* ------------------------------------------------------------------ */
/* Common tests                                                        */
/* ------------------------------------------------------------------ */

ZTEST(zstreamer_dac, test_multiple_buffers) {
  int ret;

  ret = zstreamer_source_start(numgen_dev);
  zassert_equal(ret, 0);

  k_msleep(100);
  uint32_t mid = fake_dac_get_write_count(fake_dac_dev);

  k_msleep(100);

  ret = zstreamer_source_stop(numgen_dev);
  zassert_equal(ret, 0);

  k_msleep(50);

  uint32_t final_count = fake_dac_get_write_count(fake_dac_dev);

  zassert_true(mid > 0, "no writes at midpoint");
  zassert_true(final_count > mid, "no additional writes after midpoint");
}

ZTEST(zstreamer_dac, test_restart_cycle) {
  int ret;

  for (int cycle = 0; cycle < 5; cycle++) {
    fake_dac_reset(fake_dac_dev);
    fake_dac_reset(fake_dac_8bit_dev);

    ret = zstreamer_source_start(numgen_dev);
    zassert_equal(ret, 0, "cycle %d start", cycle);

    k_msleep(100);

    ret = zstreamer_source_stop(numgen_dev);
    zassert_equal(ret, 0, "cycle %d stop", cycle);

    k_msleep(50);

    zassert_true(fake_dac_get_write_count(fake_dac_dev) > 0,
                 "cycle %d: no 12-bit DAC writes", cycle);
    zassert_true(fake_dac_get_write_count(fake_dac_8bit_dev) > 0,
                 "cycle %d: no 8-bit DAC writes", cycle);
  }
}

/* ------------------------------------------------------------------ */
/* Pool cleanup                                                        */
/* ------------------------------------------------------------------ */

ZTEST(zstreamer_dac, test_pool_free) {
  int ret;

  ret = zstreamer_source_start(numgen_dev);
  zassert_equal(ret, 0);

  k_msleep(200);

  ret = zstreamer_source_stop(numgen_dev);
  zassert_equal(ret, 0);

  assert_pool_free(graph_dev);
}
