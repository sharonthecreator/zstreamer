/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include <zstreamer/node.h>
#include <zstreamer/source.h>
#include <zstreamer_test/helpers.h>

#include <zstreamer/test/count_sink.h>

#define GRAPH_NODE      DT_NODELABEL(streaming_graph)
#define SRC_NODE        DT_NODELABEL(numgen_source)
#define FILTER_NODE     DT_NODELABEL(odd_filter)
#define TRUE_SINK_NODE  DT_NODELABEL(true_sinker)
#define FALSE_SINK_NODE DT_NODELABEL(false_sinker)

static const struct device *graph_dev = DEVICE_DT_GET(GRAPH_NODE);
static const struct device *src_dev = DEVICE_DT_GET(SRC_NODE);
static const struct device *filter_dev = DEVICE_DT_GET(FILTER_NODE);
static const struct device *true_sink_dev = DEVICE_DT_GET(TRUE_SINK_NODE);
static const struct device *false_sink_dev = DEVICE_DT_GET(FALSE_SINK_NODE);

static void cleanup(void *fixture)
{
	ARG_UNUSED(fixture);

	zstreamer_source_stop(src_dev);
	count_sink_reset(true_sink_dev);
	count_sink_reset(false_sink_dev);
}

ZTEST_SUITE(zstreamer_filter, NULL, NULL, cleanup, cleanup, NULL);

ZTEST(zstreamer_filter, test_devices_ready)
{
	zassert_true(device_is_ready(graph_dev), "graph not ready");
	zassert_true(device_is_ready(src_dev), "src not ready");
	zassert_true(device_is_ready(filter_dev), "filter not ready");
	zassert_true(device_is_ready(true_sink_dev), "true sink not ready");
	zassert_true(device_is_ready(false_sink_dev), "false sink not ready");
}

ZTEST(zstreamer_filter, test_filter_routing)
{
	int ret;
	uint32_t true_bufs, false_bufs;

	ret = zstreamer_source_start(src_dev);
	zassert_equal(ret, 0, "src start: %d", ret);

	k_msleep(200);

	ret = zstreamer_source_stop(src_dev);
	zassert_equal(ret, 0, "src stop: %d", ret);

	true_bufs = count_sink_get_buf_count(true_sink_dev);
	false_bufs = count_sink_get_buf_count(false_sink_dev);

	zassert_true(true_bufs > 0, "true path received no buffers");
	zassert_true(false_bufs > 0, "false path received no buffers");

	assert_pool_free(graph_dev);
}

ZTEST(zstreamer_filter, test_filter_balanced)
{
	int ret;
	uint32_t true_bufs, false_bufs;
	uint32_t diff;

	ret = zstreamer_source_start(src_dev);
	zassert_equal(ret, 0, "src start: %d", ret);

	k_msleep(200);

	ret = zstreamer_source_stop(src_dev);
	zassert_equal(ret, 0, "src stop: %d", ret);

	true_bufs = count_sink_get_buf_count(true_sink_dev);
	false_bufs = count_sink_get_buf_count(false_sink_dev);

	zassert_true(true_bufs > 0, "true path: no buffers");
	zassert_true(false_bufs > 0, "false path: no buffers");

	diff = (true_bufs > false_bufs) ? (true_bufs - false_bufs) : (false_bufs - true_bufs);
	zassert_true(diff <= 1, "unbalanced: true=%u false=%u diff=%u", true_bufs, false_bufs,
		     diff);

	assert_pool_free(graph_dev);
}

ZTEST(zstreamer_filter, test_filter_restart_cycle)
{
	for (int cycle = 0; cycle < 5; cycle++) {
		int ret;
		uint32_t true_bufs, false_bufs;

		count_sink_reset(true_sink_dev);
		count_sink_reset(false_sink_dev);

		ret = zstreamer_source_start(src_dev);
		zassert_equal(ret, 0, "cycle %d start: %d", cycle, ret);

		k_msleep(100);

		ret = zstreamer_source_stop(src_dev);
		zassert_equal(ret, 0, "cycle %d stop: %d", cycle, ret);

		true_bufs = count_sink_get_buf_count(true_sink_dev);
		false_bufs = count_sink_get_buf_count(false_sink_dev);

		zassert_true(true_bufs > 0, "cycle %d: true path empty", cycle);
		zassert_true(false_bufs > 0, "cycle %d: false path empty", cycle);

		assert_pool_free(graph_dev);
	}
}

ZTEST(zstreamer_filter, test_filter_high_throughput)
{
	int ret;
	uint32_t true_bufs, false_bufs, total, diff;

	ret = zstreamer_source_start(src_dev);
	zassert_equal(ret, 0, "src start: %d", ret);

	k_msleep(5000);

	ret = zstreamer_source_stop(src_dev);
	zassert_equal(ret, 0, "src stop: %d", ret);

	true_bufs = count_sink_get_buf_count(true_sink_dev);
	false_bufs = count_sink_get_buf_count(false_sink_dev);
	total = true_bufs + false_bufs;

	zassert_true(total >= 100, "expected >= 100 total bufs, got %u (true=%u false=%u)", total,
		     true_bufs, false_bufs);

	diff = (true_bufs > false_bufs) ? (true_bufs - false_bufs) : (false_bufs - true_bufs);
	zassert_true(diff <= 1, "unbalanced after high throughput: true=%u false=%u", true_bufs,
		     false_bufs);

	assert_pool_free(graph_dev);
}
