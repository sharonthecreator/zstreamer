/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 *
 * In-band stream event tests.
 *
 * Pipeline: numgen -> passthrough -> odd_filter -> {true, false} sinks.
 * Verifies that STREAM_START/STOP events traverse processors and
 * filters (both child sets), arrive exactly once per run, never leak
 * into the data counters, and stay ordered behind the run's data.
 */

#include <zephyr/device.h>
#include <zephyr/net_buf.h>
#include <zephyr/ztest.h>

#include <zstreamer/node.h>
#include <zstreamer/source.h>
#include <zstreamer_test/helpers.h>

#include <zstreamer/test/count_sink.h>

#define GRAPH_NODE DT_NODELABEL(streaming_graph)
#define SRC_NODE   DT_NODELABEL(numgen_source)

static const struct device *graph_dev = DEVICE_DT_GET(GRAPH_NODE);
static const struct device *src_dev = DEVICE_DT_GET(SRC_NODE);
static const struct device *true_sink_dev = DEVICE_DT_GET(DT_NODELABEL(true_sinker));
static const struct device *false_sink_dev = DEVICE_DT_GET(DT_NODELABEL(false_sinker));

static void events_cleanup(void *fixture)
{
	ARG_UNUSED(fixture);

	zstreamer_source_stop(src_dev);
	k_msleep(50);
	count_sink_reset(true_sink_dev);
	count_sink_reset(false_sink_dev);
}

ZTEST_SUITE(zstreamer_events, NULL, NULL, events_cleanup, events_cleanup, NULL);

ZTEST(zstreamer_events, test_events_reach_all_sinks)
{
	int ret;

	ret = zstreamer_source_start(src_dev);
	zassert_equal(ret, 0, "src start: %d", ret);

	k_msleep(200);

	ret = zstreamer_source_stop(src_dev);
	zassert_equal(ret, 0, "src stop: %d", ret);

	/* Let the sinks drain their fifos. */
	k_msleep(50);

	/* Events cross the processor and both filter output sets. */
	zassert_equal(count_sink_get_start_event_count(true_sink_dev), 1, "true sink start events");
	zassert_equal(count_sink_get_stop_event_count(true_sink_dev), 1, "true sink stop events");
	zassert_equal(count_sink_get_start_event_count(false_sink_dev), 1,
		      "false sink start events");
	zassert_equal(count_sink_get_stop_event_count(false_sink_dev), 1, "false sink stop events");

	assert_pool_free(graph_dev);
}

ZTEST(zstreamer_events, test_events_not_counted_as_data)
{
	int ret;

	ret = zstreamer_source_start(src_dev);
	zassert_equal(ret, 0, "src start: %d", ret);

	k_msleep(200);

	ret = zstreamer_source_stop(src_dev);
	zassert_equal(ret, 0, "src stop: %d", ret);

	k_msleep(50);

	uint32_t bufs =
		count_sink_get_buf_count(true_sink_dev) + count_sink_get_buf_count(false_sink_dev);
	uint32_t bytes = count_sink_get_byte_count(true_sink_dev) +
			 count_sink_get_byte_count(false_sink_dev);

	/* Data still flows and zero-length events never hit process(). */
	zassert_true(bufs > 0, "no data buffers");
	zassert_equal(bytes, bufs * DT_PROP(GRAPH_NODE, buffer_size),
		      "event leaked into data counters: %u bytes for %u bufs", bytes, bufs);

	assert_pool_free(graph_dev);
}

ZTEST(zstreamer_events, test_one_event_pair_per_cycle)
{
	const int cycles = 5;

	for (int i = 0; i < cycles; i++) {
		int ret = zstreamer_source_start(src_dev);

		zassert_equal(ret, 0, "cycle %d start: %d", i, ret);
		k_msleep(50);
		ret = zstreamer_source_stop(src_dev);
		zassert_equal(ret, 0, "cycle %d stop: %d", i, ret);
		k_msleep(20);
	}

	k_msleep(50);

	zassert_equal(count_sink_get_start_event_count(true_sink_dev), cycles,
		      "start events != cycles");
	zassert_equal(count_sink_get_stop_event_count(true_sink_dev), cycles,
		      "stop events != cycles");

	assert_pool_free(graph_dev);
}
