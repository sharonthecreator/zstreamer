/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/net_buf.h>
#include <zephyr/ztest.h>

#include <zstreamer/node.h>
#include <zstreamer_test/helpers.h>

#define GRAPH_NODE         DT_NODELABEL(streaming_graph)
#define STEREO_NODE        DT_NODELABEL(stereo_decoupler)
#define LEFT_SINK_NODE     DT_NODELABEL(left_sink)
#define LEFT_TAP_SINK_NODE DT_NODELABEL(left_tap_sink)
#define RIGHT_SINK_NODE    DT_NODELABEL(right_sink)

#define BUFFER_COUNT DT_PROP(GRAPH_NODE, buffer_count)

static const struct device *graph_dev = DEVICE_DT_GET(GRAPH_NODE);
static const struct device *stereo_dev = DEVICE_DT_GET(STEREO_NODE);
static const struct device *left_sink_dev = DEVICE_DT_GET(LEFT_SINK_NODE);
static const struct device *left_tap_sink_dev = DEVICE_DT_GET(LEFT_TAP_SINK_NODE);
static const struct device *right_sink_dev = DEVICE_DT_GET(RIGHT_SINK_NODE);

static const struct device *const sink_devs[] = {
	DEVICE_DT_GET(LEFT_SINK_NODE),
	DEVICE_DT_GET(LEFT_TAP_SINK_NODE),
	DEVICE_DT_GET(RIGHT_SINK_NODE),
};

static struct zstreamer_node_data *node_data(const struct device *dev)
{
	return (struct zstreamer_node_data *)dev->data;
}

static void feed_buf(const struct device *dev, const void *contents, size_t len)
{
	struct net_buf *buf = zstreamer_node_alloc_buf(dev, K_MSEC(100));

	zassert_not_null(buf, "buffer allocation failed");
	zassert_true(net_buf_tailroom(buf) >= len, "test input is too large");
	net_buf_add_mem(buf, contents, len);
	k_fifo_put(&node_data(dev)->fifo, buf);
}

static void feed_unaligned_buf(const struct device *dev, const void *contents, size_t len)
{
	struct net_buf *buf = zstreamer_node_alloc_buf(dev, K_MSEC(100));
	size_t reserve = 1U;

	zassert_not_null(buf, "buffer allocation failed");
	if ((((uintptr_t)buf->data + reserve) % sizeof(uint32_t)) == 0U) {
		reserve++;
	}
	net_buf_reserve(buf, reserve);
	zassert_not_equal((uintptr_t)buf->data % sizeof(uint32_t), 0U,
			  "test input is unexpectedly aligned");
	zassert_true(net_buf_tailroom(buf) >= len, "test input is too large");
	net_buf_add_mem(buf, contents, len);
	k_fifo_put(&node_data(dev)->fifo, buf);
}

static struct net_buf *get_output(const struct device *sink)
{
	struct net_buf *buf = k_fifo_get(&node_data(sink)->fifo, K_MSEC(100));

	zassert_not_null(buf, "%s received no output", sink->name);
	return buf;
}

static void drain_sink(const struct device *sink)
{
	zstreamer_node_drain_fifo(&node_data(sink)->fifo);
}

static void assert_sink_empty(const struct device *sink)
{
	struct net_buf *buf = k_fifo_get(&node_data(sink)->fifo, K_NO_WAIT);

	if (buf != NULL) {
		net_buf_unref(buf);
	}
	zassert_is_null(buf, "%s unexpectedly received a buffer", sink->name);
}

static void *decoupler_setup(void)
{
	zassert_true(device_is_ready(graph_dev), "graph not ready");
	zassert_true(device_is_ready(stereo_dev), "stereo decoupler not ready");

	for (size_t i = 0; i < ARRAY_SIZE(sink_devs); i++) {
		zassert_true(device_is_ready(sink_devs[i]), "%s not ready", sink_devs[i]->name);
		k_thread_suspend(&node_data(sink_devs[i])->thread);
	}

	return NULL;
}

static void decoupler_before(void *fixture)
{
	ARG_UNUSED(fixture);

	for (size_t i = 0; i < ARRAY_SIZE(sink_devs); i++) {
		drain_sink(sink_devs[i]);
	}
}

static void decoupler_after(void *fixture)
{
	ARG_UNUSED(fixture);

	k_msleep(20);
	for (size_t i = 0; i < ARRAY_SIZE(sink_devs); i++) {
		drain_sink(sink_devs[i]);
	}
	zstreamer_node_drain_fifo(&node_data(stereo_dev)->fifo);
	assert_pool_free(graph_dev);
}

static void decoupler_teardown(void *fixture)
{
	ARG_UNUSED(fixture);

	for (size_t i = 0; i < ARRAY_SIZE(sink_devs); i++) {
		k_thread_resume(&node_data(sink_devs[i])->thread);
	}
}

ZTEST_SUITE(zstreamer_decoupler, NULL, decoupler_setup, decoupler_before, decoupler_after,
	    decoupler_teardown);

ZTEST(zstreamer_decoupler, test_uint16_stereo_deinterleaving_and_fanout)
{
	const uint16_t interleaved[] = {
		0x1001, 0x2001, 0x1002, 0x2002, 0x1003, 0x2003, 0x1004, 0x2004,
	};
	const uint16_t expected_left[] = {0x1001, 0x1002, 0x1003, 0x1004};
	const uint16_t expected_right[] = {0x2001, 0x2002, 0x2003, 0x2004};
	struct net_buf *left;
	struct net_buf *left_tap;
	struct net_buf *right;

	feed_buf(stereo_dev, interleaved, sizeof(interleaved));
	left = get_output(left_sink_dev);
	left_tap = get_output(left_tap_sink_dev);
	right = get_output(right_sink_dev);

	zassert_equal(left->len, sizeof(expected_left), "left length mismatch");
	zassert_equal(right->len, sizeof(expected_right), "right length mismatch");
	zassert_mem_equal(left->data, expected_left, sizeof(expected_left),
			  "left samples are not deinterleaved");
	zassert_mem_equal(right->data, expected_right, sizeof(expected_right),
			  "right samples are not deinterleaved");
	zassert_equal_ptr(left, left_tap, "one channel's readonly children should share a buffer");

	net_buf_unref(left);
	net_buf_unref(left_tap);
	net_buf_unref(right);
}

ZTEST(zstreamer_decoupler, test_uint16_unaligned_input)
{
	const uint16_t interleaved[] = {0x1001, 0x2001, 0x1002, 0x2002};
	const uint16_t expected_left[] = {0x1001, 0x1002};
	const uint16_t expected_right[] = {0x2001, 0x2002};
	struct net_buf *left;
	struct net_buf *left_tap;
	struct net_buf *right;

	feed_unaligned_buf(stereo_dev, interleaved, sizeof(interleaved));
	left = get_output(left_sink_dev);
	left_tap = get_output(left_tap_sink_dev);
	right = get_output(right_sink_dev);

	zassert_mem_equal(left->data, expected_left, sizeof(expected_left),
			  "left samples are not deinterleaved");
	zassert_mem_equal(right->data, expected_right, sizeof(expected_right),
			  "right samples are not deinterleaved");

	net_buf_unref(left);
	net_buf_unref(left_tap);
	net_buf_unref(right);
}

ZTEST(zstreamer_decoupler, test_partial_frame_is_dropped)
{
	const uint8_t partial_stereo_frame[] = {0, 1, 2, 3, 4, 5};

	feed_buf(stereo_dev, partial_stereo_frame, sizeof(partial_stereo_frame));
	k_msleep(20);

	assert_sink_empty(left_sink_dev);
	assert_sink_empty(left_tap_sink_dev);
	assert_sink_empty(right_sink_dev);
}

ZTEST(zstreamer_decoupler, test_allocation_failure_does_not_publish_one_side)
{
	struct net_buf *held[BUFFER_COUNT - 2];
	const uint16_t interleaved[] = {0x1001, 0x2001};

	for (size_t i = 0; i < ARRAY_SIZE(held); i++) {
		held[i] = zstreamer_node_alloc_buf(stereo_dev, K_NO_WAIT);
		zassert_not_null(held[i], "failed to reserve pool buffer %zu", i);
	}

	/* The input consumes one of the two remaining buffers.  Only the left
	 * buffer can then be allocated, so the node must roll it back. */
	feed_buf(stereo_dev, interleaved, sizeof(interleaved));
	k_msleep(20);

	assert_sink_empty(left_sink_dev);
	assert_sink_empty(left_tap_sink_dev);
	assert_sink_empty(right_sink_dev);

	for (size_t i = 0; i < ARRAY_SIZE(held); i++) {
		net_buf_unref(held[i]);
	}
}
