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
#define PROC_NODE DT_NODELABEL(passthrough)
#define SINK_NODE DT_NODELABEL(count_sinker)

static const struct device *graph_dev = DEVICE_DT_GET(GRAPH_NODE);
static const struct device *src_dev = DEVICE_DT_GET(SRC_NODE);
static const struct device *proc_dev = DEVICE_DT_GET(PROC_NODE);
static const struct device *sink_dev = DEVICE_DT_GET(SINK_NODE);

static void cleanup(void *fixture) {
  ARG_UNUSED(fixture);

  zstreamer_source_stop(src_dev);
  count_sink_reset(sink_dev);
}

ZTEST_SUITE(zstreamer_processor, NULL, NULL, cleanup, cleanup, NULL);

ZTEST(zstreamer_processor, test_devices_ready) {
  zassert_true(device_is_ready(graph_dev), "graph not ready");
  zassert_true(device_is_ready(src_dev), "src not ready");
  zassert_true(device_is_ready(proc_dev), "processor not ready");
  zassert_true(device_is_ready(sink_dev), "sink not ready");
}

ZTEST(zstreamer_processor, test_pipeline_flow) {
  int ret;
  uint32_t bufs;

  ret = zstreamer_source_start(src_dev);
  zassert_equal(ret, 0, "src start: %d", ret);

  k_msleep(200);

  ret = zstreamer_source_stop(src_dev);
  zassert_equal(ret, 0, "src stop: %d", ret);

  bufs = count_sink_get_buf_count(sink_dev);
  zassert_true(bufs > 0, "no buffers through processor, got %u", bufs);
}

ZTEST(zstreamer_processor, test_processor_restart_cycle) {
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
