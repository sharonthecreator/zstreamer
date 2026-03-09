/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 *
 * ADC source zstreamer node driver tests.
 *
 * Two sources share one count_sink (tested one at a time):
 *   12-bit: adc_source   (ch0, 12-bit) → counter
 *   8-bit:  adc_source_8bit (ch1, 8-bit) → counter
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/adc/adc_emul.h>
#include <zephyr/net_buf.h>
#include <zephyr/ztest.h>

#include <zstreamer/node.h>
#include <zstreamer/source.h>
#include <zstreamer_test/helpers.h>

#include <zstreamer/test/count_sink.h>

/* ------------------------------------------------------------------ */
/* DTS nodes                                                           */
/* ------------------------------------------------------------------ */

#define GRAPH_NODE         DT_NODELABEL(adc_streaming_graph)
#define ADC_SRC_NODE       DT_NODELABEL(adc_source)
#define ADC_SRC_8BIT_NODE  DT_NODELABEL(adc_source_8bit)
#define COUNT_NODE         DT_NODELABEL(counter)
#define ADC_EMUL_NODE      DT_NODELABEL(adc_emul)

static const struct device *graph_dev       = DEVICE_DT_GET(GRAPH_NODE);
static const struct device *adc_src_dev     = DEVICE_DT_GET(ADC_SRC_NODE);
static const struct device *adc_src_8bit_dev = DEVICE_DT_GET(ADC_SRC_8BIT_NODE);
static const struct device *count_dev       = DEVICE_DT_GET(COUNT_NODE);
static const struct device *adc_emul_dev    = DEVICE_DT_GET(ADC_EMUL_NODE);

/* ------------------------------------------------------------------ */
/* Fixture                                                             */
/* ------------------------------------------------------------------ */

static void cleanup(void *fixture) {
  ARG_UNUSED(fixture);

  zstreamer_source_stop(adc_src_dev);
  zstreamer_source_stop(adc_src_8bit_dev);
  k_msleep(50);
  count_sink_reset(count_dev);
}

ZTEST_SUITE(zstreamer_adc, NULL, NULL, cleanup, cleanup, NULL);

/* ------------------------------------------------------------------ */
/* Basic tests                                                         */
/* ------------------------------------------------------------------ */

ZTEST(zstreamer_adc, test_devices_ready) {
  zassert_true(device_is_ready(graph_dev), "graph not ready");
  zassert_true(device_is_ready(adc_src_dev), "adc_src not ready");
  zassert_true(device_is_ready(adc_src_8bit_dev), "adc_src_8bit not ready");
  zassert_true(device_is_ready(count_dev), "count_sink not ready");
  zassert_true(device_is_ready(adc_emul_dev), "adc_emul not ready");
}

ZTEST(zstreamer_adc, test_src_start_stop) {
  int ret;

  ret = zstreamer_source_start(adc_src_dev);
  zassert_equal(ret, 0, "src start failed: %d", ret);

  ret = zstreamer_source_start(adc_src_dev);
  zassert_equal(ret, -EALREADY, "double start: %d", ret);

  ret = zstreamer_source_stop(adc_src_dev);
  zassert_equal(ret, 0, "src stop failed: %d", ret);

  ret = zstreamer_source_stop(adc_src_dev);
  zassert_equal(ret, -EALREADY, "double stop: %d", ret);
}

ZTEST(zstreamer_adc, test_src_buf_alloc) {
  struct net_buf *buf;

  buf = zstreamer_node_alloc_buf(adc_src_dev, K_MSEC(100));
  zassert_not_null(buf, "buf alloc failed");
  zassert_true(net_buf_tailroom(buf) > 0, "no tailroom");
  net_buf_unref(buf);
}

/* ------------------------------------------------------------------ */
/* 12-bit source tests                                                 */
/* ------------------------------------------------------------------ */

ZTEST(zstreamer_adc, test_src_16bit_produces_data) {
  int ret;

  ret = adc_emul_const_value_set(adc_emul_dev, 0, 1234);
  zassert_equal(ret, 0, "adc_emul_const_value_set failed: %d", ret);

  ret = zstreamer_source_start(adc_src_dev);
  zassert_equal(ret, 0);

  k_msleep(200);

  ret = zstreamer_source_stop(adc_src_dev);
  zassert_equal(ret, 0);

  uint32_t bufs = count_sink_get_buf_count(count_dev);
  uint32_t bytes = count_sink_get_byte_count(count_dev);

  zassert_true(bufs > 0, "no buffers received");
  zassert_true(bytes > 0, "no bytes received");
  zassert_true(bytes % sizeof(int16_t) == 0,
               "byte count %u not aligned to 16-bit samples", bytes);
}

ZTEST(zstreamer_adc, test_src_16bit_frame_size) {
  int ret;

  ret = adc_emul_const_value_set(adc_emul_dev, 0, 1000);
  zassert_equal(ret, 0);

  ret = zstreamer_source_start(adc_src_dev);
  zassert_equal(ret, 0);

  k_msleep(200);

  ret = zstreamer_source_stop(adc_src_dev);
  zassert_equal(ret, 0);

  uint32_t bufs = count_sink_get_buf_count(count_dev);
  uint32_t bytes = count_sink_get_byte_count(count_dev);

  zassert_true(bufs > 0, "no buffers");

  /* 16 samples × 1 channel × 2 bytes = 32 bytes per buffer. */
  uint32_t expected_frame = 16 * 1 * sizeof(uint16_t);

  zassert_equal(bytes / bufs, expected_frame,
                "frame size %u != expected %u", bytes / bufs, expected_frame);
  zassert_equal(bytes % expected_frame, 0,
                "total bytes %u not a multiple of frame size %u", bytes,
                expected_frame);
}

ZTEST(zstreamer_adc, test_src_16bit_restart_cycle) {
  int ret;

  ret = adc_emul_const_value_set(adc_emul_dev, 0, 500);
  zassert_equal(ret, 0);

  for (int cycle = 0; cycle < 5; cycle++) {
    count_sink_reset(count_dev);

    ret = zstreamer_source_start(adc_src_dev);
    zassert_equal(ret, 0, "cycle %d start", cycle);

    k_msleep(100);

    ret = zstreamer_source_stop(adc_src_dev);
    zassert_equal(ret, 0, "cycle %d stop", cycle);

    zassert_true(count_sink_get_buf_count(count_dev) > 0,
                 "cycle %d: no buffers", cycle);

    k_msleep(50);
  }
}

/* ------------------------------------------------------------------ */
/* 8-bit source tests                                                  */
/* ------------------------------------------------------------------ */

ZTEST(zstreamer_adc, test_src_8bit_produces_data) {
  int ret;

  ret = adc_emul_const_value_set(adc_emul_dev, 1, 42);
  zassert_equal(ret, 0, "adc_emul_const_value_set ch1 failed: %d", ret);

  ret = zstreamer_source_start(adc_src_8bit_dev);
  zassert_equal(ret, 0);

  k_msleep(200);

  ret = zstreamer_source_stop(adc_src_8bit_dev);
  zassert_equal(ret, 0);

  uint32_t bufs = count_sink_get_buf_count(count_dev);
  uint32_t bytes = count_sink_get_byte_count(count_dev);

  zassert_true(bufs > 0, "no buffers received");
  zassert_true(bytes > 0, "no bytes received");
  /* 8-bit source packs 1 byte per sample — any byte count is valid. */
}

ZTEST(zstreamer_adc, test_src_8bit_frame_size) {
  int ret;

  ret = adc_emul_const_value_set(adc_emul_dev, 1, 42);
  zassert_equal(ret, 0);

  ret = zstreamer_source_start(adc_src_8bit_dev);
  zassert_equal(ret, 0);

  k_msleep(200);

  ret = zstreamer_source_stop(adc_src_8bit_dev);
  zassert_equal(ret, 0);

  uint32_t bufs = count_sink_get_buf_count(count_dev);
  uint32_t bytes = count_sink_get_byte_count(count_dev);

  zassert_true(bufs > 0, "no buffers");

  /* 16 samples × 1 channel × 1 byte = 16 bytes per buffer. */
  uint32_t expected_frame = 16 * 1 * sizeof(uint8_t);

  zassert_equal(bytes / bufs, expected_frame,
                "frame size %u != expected %u", bytes / bufs, expected_frame);
  zassert_equal(bytes % expected_frame, 0,
                "total bytes %u not a multiple of frame size %u", bytes,
                expected_frame);
}

ZTEST(zstreamer_adc, test_src_8bit_sample_size) {
  int ret;

  ret = adc_emul_const_value_set(adc_emul_dev, 1, 100);
  zassert_equal(ret, 0);

  ret = zstreamer_source_start(adc_src_8bit_dev);
  zassert_equal(ret, 0);

  k_msleep(200);

  ret = zstreamer_source_stop(adc_src_8bit_dev);
  zassert_equal(ret, 0);

  uint32_t bytes_8 = count_sink_get_byte_count(count_dev);

  /* Now run 16-bit source for the same duration. */
  count_sink_reset(count_dev);

  ret = adc_emul_const_value_set(adc_emul_dev, 0, 100);
  zassert_equal(ret, 0);

  ret = zstreamer_source_start(adc_src_dev);
  zassert_equal(ret, 0);

  k_msleep(200);

  ret = zstreamer_source_stop(adc_src_dev);
  zassert_equal(ret, 0);

  uint32_t bytes_16 = count_sink_get_byte_count(count_dev);

  /*
   * Both sources use the same sample rate and samples config.
   * The 16-bit source should produce ~2x the bytes of the 8-bit source
   * for the same number of samples.
   */
  zassert_true(bytes_8 > 0, "8-bit: no bytes");
  zassert_true(bytes_16 > 0, "16-bit: no bytes");
  zassert_true(bytes_16 > bytes_8,
               "16-bit bytes (%u) should exceed 8-bit bytes (%u)", bytes_16,
               bytes_8);
}

ZTEST(zstreamer_adc, test_src_8bit_restart_cycle) {
  int ret;

  ret = adc_emul_const_value_set(adc_emul_dev, 1, 50);
  zassert_equal(ret, 0);

  for (int cycle = 0; cycle < 5; cycle++) {
    count_sink_reset(count_dev);

    ret = zstreamer_source_start(adc_src_8bit_dev);
    zassert_equal(ret, 0, "cycle %d start", cycle);

    k_msleep(100);

    ret = zstreamer_source_stop(adc_src_8bit_dev);
    zassert_equal(ret, 0, "cycle %d stop", cycle);

    zassert_true(count_sink_get_buf_count(count_dev) > 0,
                 "cycle %d: no buffers", cycle);

    k_msleep(50);
  }
}

/* ------------------------------------------------------------------ */
/* Pool cleanup                                                        */
/* ------------------------------------------------------------------ */

ZTEST(zstreamer_adc, test_pool_free) {
  int ret;

  ret = adc_emul_const_value_set(adc_emul_dev, 0, 100);
  zassert_equal(ret, 0);
  ret = adc_emul_const_value_set(adc_emul_dev, 1, 100);
  zassert_equal(ret, 0);

  ret = zstreamer_source_start(adc_src_dev);
  zassert_equal(ret, 0);
  ret = zstreamer_source_start(adc_src_8bit_dev);
  zassert_equal(ret, 0);

  k_msleep(200);

  ret = zstreamer_source_stop(adc_src_dev);
  zassert_equal(ret, 0);
  ret = zstreamer_source_stop(adc_src_8bit_dev);
  zassert_equal(ret, 0);

  assert_pool_free(graph_dev);
}
