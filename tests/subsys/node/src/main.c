/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/net_buf.h>
#include <zephyr/ztest.h>

#include <zstreamer/node.h>
#include <zstreamer/source.h>
#include <zstreamer_test/helpers.h>

#include <zstreamer/test/count_sink.h>

/* ------------------------------------------------------------------ */
/* Pipeline: numgen → passthrough → count_sinker  (64-byte bufs)      */
/* ------------------------------------------------------------------ */

#define GRAPH_NODE DT_NODELABEL(streaming_graph)
#define SRC_NODE DT_NODELABEL(numgen_source)
#define PROC_NODE DT_NODELABEL(passthrough)
#define SINK_NODE DT_NODELABEL(count_sinker)

#define BUFFER_SIZE DT_PROP(GRAPH_NODE, buffer_size)
#define NUMGEN_SLEEP_MS DT_PROP(SRC_NODE, sleep_ms)
#define TICK_MS (1000 / CONFIG_SYS_CLOCK_TICKS_PER_SEC)

static const struct device *graph_dev = DEVICE_DT_GET(GRAPH_NODE);
static const struct device *src_dev = DEVICE_DT_GET(SRC_NODE);
static const struct device *proc_dev = DEVICE_DT_GET(PROC_NODE);
static const struct device *sink_dev = DEVICE_DT_GET(SINK_NODE);

static void node_cleanup(void *fixture) {
  ARG_UNUSED(fixture);

  zstreamer_source_stop(src_dev);
  count_sink_reset(sink_dev);
}

ZTEST_SUITE(zstreamer_node, NULL, NULL, node_cleanup, node_cleanup, NULL);

ZTEST(zstreamer_node, test_devices_ready) {
  zassert_true(device_is_ready(graph_dev), "graph not ready");
  zassert_true(device_is_ready(src_dev), "src not ready");
  zassert_true(device_is_ready(proc_dev), "processor not ready");
  zassert_true(device_is_ready(sink_dev), "sink not ready");
}

ZTEST(zstreamer_node, test_start_stop) {
  int ret;

  ret = zstreamer_source_start(src_dev);
  zassert_equal(ret, 0, "src start failed: %d", ret);

  ret = zstreamer_source_start(src_dev);
  zassert_equal(ret, -EALREADY, "double start: %d", ret);

  ret = zstreamer_source_stop(src_dev);
  zassert_equal(ret, 0, "src stop failed: %d", ret);

  ret = zstreamer_source_stop(src_dev);
  zassert_equal(ret, -EALREADY, "double stop: %d", ret);
}

ZTEST(zstreamer_node, test_buf_alloc) {
  struct net_buf *buf;

  buf = zstreamer_node_alloc_buf(src_dev, K_MSEC(100));
  zassert_not_null(buf, "buf alloc failed");

  zassert_true(net_buf_tailroom(buf) > 0, "no tailroom");
  net_buf_unref(buf);
}

ZTEST(zstreamer_node, test_pipeline_flow) {
  int ret;
  uint32_t bufs;

  ret = zstreamer_source_start(src_dev);
  zassert_equal(ret, 0, "src start: %d", ret);

  k_msleep(200);

  ret = zstreamer_source_stop(src_dev);
  zassert_equal(ret, 0, "src stop: %d", ret);

  bufs = count_sink_get_buf_count(sink_dev);
  zassert_true(bufs > 0, "no buffers through processor, got %u", bufs);

  assert_pool_free(graph_dev);
}

ZTEST(zstreamer_node, test_byte_count) {
  int ret;
  uint32_t bufs, bytes;

  ret = zstreamer_source_start(src_dev);
  zassert_equal(ret, 0, "src start: %d", ret);

  k_msleep(200);

  ret = zstreamer_source_stop(src_dev);
  zassert_equal(ret, 0, "src stop: %d", ret);

  bufs = count_sink_get_buf_count(sink_dev);
  bytes = count_sink_get_byte_count(sink_dev);

  zassert_true(bufs > 0, "no buffers received");
  zassert_equal(bytes, bufs * BUFFER_SIZE,
                "bytes %u != bufs %u * buffer_size %u", bytes, bufs,
                BUFFER_SIZE);

  assert_pool_free(graph_dev);
}

ZTEST(zstreamer_node, test_restart_cycle) {
  for (int cycle = 0; cycle < 5; cycle++) {
    int ret;
    uint32_t bufs;

    count_sink_reset(sink_dev);

    ret = zstreamer_source_start(src_dev);
    zassert_equal(ret, 0, "cycle %d start: %d", cycle, ret);

    k_msleep(100);

    ret = zstreamer_source_stop(src_dev);
    zassert_equal(ret, 0, "cycle %d stop: %d", cycle, ret);

    bufs = count_sink_get_buf_count(sink_dev);
    zassert_true(bufs > 0, "cycle %d: no buffers", cycle);

    assert_pool_free(graph_dev);
  }
}

ZTEST(zstreamer_node, test_high_throughput) {
  int ret;
  uint32_t bufs, bytes;

  ret = zstreamer_source_start(src_dev);
  zassert_equal(ret, 0, "src start: %d", ret);

  k_msleep(5000);

  ret = zstreamer_source_stop(src_dev);
  zassert_equal(ret, 0, "src stop: %d", ret);

  bufs = count_sink_get_buf_count(sink_dev);
  bytes = count_sink_get_byte_count(sink_dev);

  zassert_true(bufs >= 100,
               "expected >= 100 bufs through processor, got %u", bufs);
  zassert_equal(bytes, bufs * BUFFER_SIZE,
                "byte count %u != bufs %u * %u", bytes, bufs, BUFFER_SIZE);

  assert_pool_free(graph_dev);
}

ZTEST(zstreamer_node, test_sustained_restart) {
  uint32_t total_bufs = 0;

  for (int cycle = 0; cycle < 30; cycle++) {
    int ret;

    count_sink_reset(sink_dev);

    ret = zstreamer_source_start(src_dev);
    zassert_equal(ret, 0, "cycle %d start: %d", cycle, ret);

    k_msleep(50);

    ret = zstreamer_source_stop(src_dev);
    zassert_equal(ret, 0, "cycle %d stop: %d", cycle, ret);

    total_bufs += count_sink_get_buf_count(sink_dev);

    assert_pool_free(graph_dev);
  }

  zassert_true(total_bufs >= 30,
               "expected >= 30 total bufs across 30 cycles, got %u",
               total_bufs);
}

ZTEST(zstreamer_node, test_buffer_rate_matches_interval) {
  int ret;
  uint32_t bufs;
  const uint32_t run_ms = 2000;
  const uint32_t cycle_ms = NUMGEN_SLEEP_MS + TICK_MS;
  const uint32_t expected = run_ms / cycle_ms;

  ret = zstreamer_source_start(src_dev);
  zassert_equal(ret, 0, "src start: %d", ret);

  k_msleep(run_ms);

  ret = zstreamer_source_stop(src_dev);
  zassert_equal(ret, 0, "src stop: %d", ret);

  bufs = count_sink_get_buf_count(sink_dev);
  zassert_true(bufs >= (uint32_t)(expected * 0.9),
               "rate too low: expected >= %u bufs (90%% of %u), got %u",
               (uint32_t)(expected * 0.9), expected, bufs);
  zassert_true(bufs <= expected + 1,
               "got %u bufs, exceeds expected %u", bufs, expected);
}

ZTEST(zstreamer_node, test_low_pipeline_overhead) {
  int ret;
  uint32_t bufs;
  const uint32_t run_ms = 5000;
  const uint32_t expected = run_ms / (NUMGEN_SLEEP_MS + TICK_MS);

  ret = zstreamer_source_start(src_dev);
  zassert_equal(ret, 0, "src start: %d", ret);

  k_msleep(run_ms);

  ret = zstreamer_source_stop(src_dev);
  zassert_equal(ret, 0, "src stop: %d", ret);

  bufs = count_sink_get_buf_count(sink_dev);

  uint32_t overhead_ms = (run_ms / bufs) - NUMGEN_SLEEP_MS;

  zassert_true(bufs >= (uint32_t)(expected * 0.9),
               "pipeline overhead too high: got %u bufs, expected >= %u",
               bufs, (uint32_t)(expected * 0.9));
  zassert_true(overhead_ms <= TICK_MS,
               "per-buffer overhead %u ms exceeds 1 tick (%u ms)",
               overhead_ms, TICK_MS);
}

