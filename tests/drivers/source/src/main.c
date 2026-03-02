/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/net_buf.h>
#include <zephyr/ztest.h>

#include <zstreamer/node.h>
#include <zstreamer/source.h>

#include "count_sink.h"

#define GRAPH_NODE DT_NODELABEL(streaming_graph)
#define SRC_NODE DT_NODELABEL(numgen_source)
#define SINK_NODE DT_NODELABEL(count_sinker)

static const struct device *graph_dev = DEVICE_DT_GET(GRAPH_NODE);
static const struct device *src_dev = DEVICE_DT_GET(SRC_NODE);
static const struct device *sink_dev = DEVICE_DT_GET(SINK_NODE);

static void cleanup(void *fixture) {
  ARG_UNUSED(fixture);

  zstreamer_source_stop(src_dev);
  count_sink_reset(sink_dev);
}

ZTEST_SUITE(zstreamer_source, NULL, NULL, cleanup, cleanup, NULL);

ZTEST(zstreamer_source, test_devices_ready) {
  zassert_true(device_is_ready(graph_dev), "graph not ready");
  zassert_true(device_is_ready(src_dev), "src not ready");
  zassert_true(device_is_ready(sink_dev), "sink not ready");
}

ZTEST(zstreamer_source, test_start_stop) {
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

ZTEST(zstreamer_source, test_buf_alloc) {
  struct net_buf *buf;

  buf = zstreamer_node_alloc_buf(src_dev, K_MSEC(100));
  zassert_not_null(buf, "buf alloc failed");

  zassert_true(net_buf_tailroom(buf) > 0, "no tailroom");
  net_buf_unref(buf);
}

ZTEST(zstreamer_source, test_data_flow) {
  int ret;
  uint32_t bufs;

  ret = zstreamer_source_start(src_dev);
  zassert_equal(ret, 0, "src start: %d", ret);

  k_msleep(200);

  ret = zstreamer_source_stop(src_dev);
  zassert_equal(ret, 0, "src stop: %d", ret);

  bufs = count_sink_get_buf_count(sink_dev);
  zassert_true(bufs > 0, "expected buffers, got %u", bufs);
}

ZTEST(zstreamer_source, test_restart_cycle) {
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
  }
}

ZTEST(zstreamer_source, test_high_throughput) {
  int ret;
  uint32_t bufs, bytes;

  ret = zstreamer_source_start(src_dev);
  zassert_equal(ret, 0, "src start: %d", ret);

  k_msleep(2000);

  ret = zstreamer_source_stop(src_dev);
  zassert_equal(ret, 0, "src stop: %d", ret);

  bufs = count_sink_get_buf_count(sink_dev);
  bytes = count_sink_get_byte_count(sink_dev);

  zassert_true(bufs >= 50, "expected >= 50 bufs, got %u", bufs);
  zassert_true(bytes >= 3200, "expected >= 3200 bytes, got %u", bytes);
}

ZTEST(zstreamer_source, test_sustained_restart) {
  for (int cycle = 0; cycle < 20; cycle++) {
    int ret;
    uint32_t bufs;

    count_sink_reset(sink_dev);

    ret = zstreamer_source_start(src_dev);
    zassert_equal(ret, 0, "cycle %d start: %d", cycle, ret);

    k_msleep(50);

    ret = zstreamer_source_stop(src_dev);
    zassert_equal(ret, 0, "cycle %d stop: %d", cycle, ret);

    bufs = count_sink_get_buf_count(sink_dev);
    zassert_true(bufs > 0, "cycle %d: no buffers", cycle);
  }
}
