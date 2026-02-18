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
#define BUFFER_SIZE 64

static const struct device *graph_dev = DEVICE_DT_GET(GRAPH_NODE);
static const struct device *src_dev = DEVICE_DT_GET(SRC_NODE);
static const struct device *sink_dev = DEVICE_DT_GET(SINK_NODE);

static void cleanup(void *fixture) {
  ARG_UNUSED(fixture);

  zstreamer_source_stop(src_dev);
  count_sink_reset(sink_dev);
}

ZTEST_SUITE(zstreamer_sink, NULL, NULL, cleanup, cleanup, NULL);

ZTEST(zstreamer_sink, test_devices_ready) {
  zassert_true(device_is_ready(graph_dev), "graph not ready");
  zassert_true(device_is_ready(src_dev), "src not ready");
  zassert_true(device_is_ready(sink_dev), "sink not ready");
}

ZTEST(zstreamer_sink, test_buf_alloc) {
  struct net_buf *buf;

  buf = zstreamer_node_alloc_buf(src_dev, K_MSEC(100));
  zassert_not_null(buf, "buf alloc failed");

  zassert_true(net_buf_tailroom(buf) > 0, "no tailroom");
  net_buf_unref(buf);
}

ZTEST(zstreamer_sink, test_sink_processes_data) {
  int ret;
  uint32_t bufs;

  ret = zstreamer_source_start(src_dev);
  zassert_equal(ret, 0, "src start: %d", ret);

  k_msleep(200);

  ret = zstreamer_source_stop(src_dev);
  zassert_equal(ret, 0, "src stop: %d", ret);

  bufs = count_sink_get_buf_count(sink_dev);
  zassert_true(bufs > 0, "sink received no buffers");
}

ZTEST(zstreamer_sink, test_sink_byte_count) {
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
}
