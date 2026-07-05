/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/net_buf.h>
#include <zephyr/ztest.h>

#include <zstreamer/node.h>
#include <zstreamer_test/helpers.h>

#include <zstreamer/test/count_sink.h>

/* ------------------------------------------------------------------ */
/* Pipeline: (test-injected bufs) → batcher → count_sinker            */
/* ------------------------------------------------------------------ */

#define GRAPH_NODE DT_NODELABEL(streaming_graph)
#define BATCH_NODE DT_NODELABEL(batcher)
#define SINK_NODE  DT_NODELABEL(count_sinker)

#define BUFFER_SIZE DT_PROP(GRAPH_NODE, buffer_size)
#define BATCH_COUNT DT_PROP(BATCH_NODE, batch_count)

static const struct device *graph_dev = DEVICE_DT_GET(GRAPH_NODE);
static const struct device *batch_dev = DEVICE_DT_GET(BATCH_NODE);
static const struct device *sink_dev = DEVICE_DT_GET(SINK_NODE);

/* Inject a full buffer into the batch node's inbound fifo, exactly as
 * an upstream node's distribute would. */
static void feed_buf(void)
{
	struct zstreamer_node_data *data = batch_dev->data;
	struct net_buf *buf = zstreamer_node_alloc_buf(batch_dev, K_MSEC(100));

	zassert_not_null(buf, "buf alloc failed");
	memset(net_buf_add(buf, BUFFER_SIZE), 0xa5, BUFFER_SIZE);
	k_fifo_put(&data->fifo, buf);
}

static void batch_cleanup(void *fixture)
{
	ARG_UNUSED(fixture);

	count_sink_reset(sink_dev);
}

ZTEST_SUITE(zstreamer_batch_node, NULL, NULL, batch_cleanup, batch_cleanup, NULL);

ZTEST(zstreamer_batch_node, test_devices_ready)
{
	zassert_true(device_is_ready(graph_dev), "graph not ready");
	zassert_true(device_is_ready(batch_dev), "batch node not ready");
	zassert_true(device_is_ready(sink_dev), "sink not ready");
}

ZTEST(zstreamer_batch_node, test_holds_until_batch_full)
{
	for (int i = 0; i < BATCH_COUNT - 1; i++) {
		feed_buf();
	}
	k_msleep(50);
	zassert_equal(count_sink_get_buf_count(sink_dev), 0,
		      "sink got buffers before the batch was full");

	feed_buf();
	k_msleep(50);
	zassert_equal(count_sink_get_buf_count(sink_dev), BATCH_COUNT,
		      "expected the full batch, got %u", count_sink_get_buf_count(sink_dev));
	zassert_equal(count_sink_get_byte_count(sink_dev), BATCH_COUNT * BUFFER_SIZE,
		      "byte count mismatch");

	assert_pool_free(graph_dev);
}

ZTEST(zstreamer_batch_node, test_repeated_batches)
{
	for (int batch = 1; batch <= 2; batch++) {
		for (int i = 0; i < BATCH_COUNT; i++) {
			feed_buf();
		}
		k_msleep(50);
		zassert_equal(count_sink_get_buf_count(sink_dev), batch * BATCH_COUNT,
			      "batch %d: expected %u bufs, got %u", batch, batch * BATCH_COUNT,
			      count_sink_get_buf_count(sink_dev));
	}

	assert_pool_free(graph_dev);
}
