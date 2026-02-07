/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/serial/uart_emul.h>
#include <zephyr/net_buf.h>

#include <zstreamer/zstnode.h>

#define GRAPH_NODE      DT_NODELABEL(streaming_graph)
#define SRC_NODE        DT_NODELABEL(uart_source)
#define SINK_NODE       DT_NODELABEL(uart_sinker)
#define UART_SRC_NODE   DT_NODELABEL(uart_src)
#define UART_SINK_NODE  DT_NODELABEL(uart_sink)
#define NUMGEN_SRC_NODE DT_NODELABEL(numgen_source)
#define FAKESINK_NODE   DT_NODELABEL(fake_sinker)

static const struct device *graph_dev = DEVICE_DT_GET(GRAPH_NODE);
static const struct device *src_dev = DEVICE_DT_GET(SRC_NODE);
static const struct device *sink_dev = DEVICE_DT_GET(SINK_NODE);
static const struct device *uart_src_dev = DEVICE_DT_GET(UART_SRC_NODE);
static const struct device *uart_sink_dev = DEVICE_DT_GET(UART_SINK_NODE);
static const struct device *numgen_src_dev = DEVICE_DT_GET(NUMGEN_SRC_NODE);
static const struct device *fakesink_dev = DEVICE_DT_GET(FAKESINK_NODE);

/* Drain any stale data and stop the source between tests. */
static void cleanup(void *fixture)
{
	uint8_t tmp[256];

	ARG_UNUSED(fixture);

	zstnode_stop(src_dev);
	uart_emul_flush_rx_data(uart_src_dev);
	uart_emul_flush_tx_data(uart_sink_dev);
	/* Consume anything the sink already wrote. */
	uart_emul_get_tx_data(uart_sink_dev, tmp, sizeof(tmp));
}

ZTEST_SUITE(zstnode_uart, NULL, NULL, cleanup, cleanup, NULL);

/* ------------------------------------------------------------------ */
/* Basic functional tests                                              */
/* ------------------------------------------------------------------ */

ZTEST(zstnode_uart, test_devices_ready)
{
	zassert_true(device_is_ready(graph_dev), "graph not ready");
	zassert_true(device_is_ready(src_dev), "src not ready");
	zassert_true(device_is_ready(sink_dev), "sink not ready");
	zassert_true(device_is_ready(uart_src_dev), "uart src not ready");
	zassert_true(device_is_ready(uart_sink_dev), "uart sink not ready");
}

ZTEST(zstnode_uart, test_start_stop)
{
	int ret;

	/* Sink start/stop must return -ENOTSUP (non-source). */
	ret = zstnode_start(sink_dev);
	zassert_equal(ret, -ENOTSUP, "sink start: %d", ret);

	ret = zstnode_stop(sink_dev);
	zassert_equal(ret, -ENOTSUP, "sink stop: %d", ret);

	/* Source start/stop. */
	ret = zstnode_start(src_dev);
	zassert_equal(ret, 0, "src start failed: %d", ret);

	/* Starting again must return -EALREADY. */
	ret = zstnode_start(src_dev);
	zassert_equal(ret, -EALREADY, "double start: %d", ret);

	ret = zstnode_stop(src_dev);
	zassert_equal(ret, 0, "src stop failed: %d", ret);

	/* Stopping again must return -EALREADY. */
	ret = zstnode_stop(src_dev);
	zassert_equal(ret, -EALREADY, "double stop: %d", ret);
}

ZTEST(zstnode_uart, test_buf_alloc)
{
	struct net_buf *buf;

	buf = zstnode_alloc_buf(src_dev, K_MSEC(100));
	zassert_not_null(buf, "buf alloc failed");

	zassert_true(net_buf_tailroom(buf) > 0, "no tailroom");
	net_buf_unref(buf);
}

ZTEST(zstnode_uart, test_uart_relay)
{
	const uint8_t tx_data[] = "hello";
	const size_t tx_len = sizeof(tx_data) - 1;
	uint8_t rx_buf[32];
	uint32_t rx_len;
	int ret;

	ret = zstnode_start(src_dev);
	zassert_equal(ret, 0, "src start: %d", ret);

	uart_emul_put_rx_data(uart_src_dev, tx_data, tx_len);

	k_msleep(500);

	ret = zstnode_stop(src_dev);
	zassert_equal(ret, 0);

	rx_len = uart_emul_get_tx_data(uart_sink_dev, rx_buf, sizeof(rx_buf));
	zassert_equal(rx_len, tx_len,
		      "expected %u bytes, got %u", tx_len, rx_len);
	zassert_mem_equal(rx_buf, tx_data, tx_len, "relayed data mismatch");
}

/* ------------------------------------------------------------------ */
/* Stress / throughput tests                                           */
/* ------------------------------------------------------------------ */

ZTEST(zstnode_uart, test_long_transfer)
{
	/* Send a large block that spans many buffers (buf-size is 64). */
	uint8_t tx_data[200];
	uint8_t rx_buf[256];
	uint32_t rx_total = 0;
	int ret;

	for (size_t i = 0; i < sizeof(tx_data); i++) {
		tx_data[i] = (uint8_t)(i & 0xff);
	}

	ret = zstnode_start(src_dev);
	zassert_equal(ret, 0);

	uart_emul_put_rx_data(uart_src_dev, tx_data, sizeof(tx_data));

	/* Allow more time for multiple buffer cycles. */
	k_msleep(1000);

	ret = zstnode_stop(src_dev);
	zassert_equal(ret, 0);

	rx_total = uart_emul_get_tx_data(uart_sink_dev, rx_buf, sizeof(rx_buf));
	zassert_equal(rx_total, sizeof(tx_data),
		      "long xfer: expected %u, got %u",
		      (unsigned)sizeof(tx_data), rx_total);
	zassert_mem_equal(rx_buf, tx_data, sizeof(tx_data));
}

ZTEST(zstnode_uart, test_burst_writes)
{
	/* Feed data in several small bursts while the pipeline is running. */
	const char *msgs[] = {"aaa", "bbb", "ccc", "ddd", "eee"};
	const size_t msg_len = 3;
	const size_t n_msgs = ARRAY_SIZE(msgs);
	uint8_t rx_buf[64];
	uint8_t expected[64];
	uint32_t rx_total;
	int ret;

	for (size_t i = 0; i < n_msgs; i++) {
		memcpy(expected + i * msg_len, msgs[i], msg_len);
	}

	ret = zstnode_start(src_dev);
	zassert_equal(ret, 0);

	for (size_t i = 0; i < n_msgs; i++) {
		uart_emul_put_rx_data(uart_src_dev,
				      (const uint8_t *)msgs[i], msg_len);
		k_msleep(50);
	}

	k_msleep(500);

	ret = zstnode_stop(src_dev);
	zassert_equal(ret, 0);

	rx_total = uart_emul_get_tx_data(uart_sink_dev, rx_buf, sizeof(rx_buf));
	zassert_equal(rx_total, n_msgs * msg_len,
		      "burst: expected %u, got %u",
		      (unsigned)(n_msgs * msg_len), rx_total);
	zassert_mem_equal(rx_buf, expected, n_msgs * msg_len);
}

ZTEST(zstnode_uart, test_restart_cycle)
{
	/* Start, transfer, stop, repeat — make sure state is clean. */
	const uint8_t pattern[] = "XY";
	uint8_t rx_buf[8];
	uint32_t rx_len;

	for (int cycle = 0; cycle < 5; cycle++) {
		int ret;

		ret = zstnode_start(src_dev);
		zassert_equal(ret, 0, "cycle %d src start", cycle);

		uart_emul_put_rx_data(uart_src_dev, pattern, 2);
		k_msleep(200);

		ret = zstnode_stop(src_dev);
		zassert_equal(ret, 0);

		rx_len = uart_emul_get_tx_data(uart_sink_dev,
					       rx_buf, sizeof(rx_buf));
		zassert_equal(rx_len, 2,
			      "cycle %d: expected 2, got %u", cycle, rx_len);
		zassert_mem_equal(rx_buf, pattern, 2);
	}
}

ZTEST(zstnode_uart, test_throughput_stress)
{
	/*
	 * Push as much data as possible: fill the emul RX FIFO to its
	 * default capacity (256 bytes) and verify everything arrives.
	 */
	uint8_t tx_data[256];
	uint8_t rx_buf[300];
	uint32_t rx_total;
	int ret;

	for (size_t i = 0; i < sizeof(tx_data); i++) {
		tx_data[i] = (uint8_t)((i * 7 + 13) & 0xff);
	}

	ret = zstnode_start(src_dev);
	zassert_equal(ret, 0);

	uart_emul_put_rx_data(uart_src_dev, tx_data, sizeof(tx_data));

	k_msleep(2000);

	ret = zstnode_stop(src_dev);
	zassert_equal(ret, 0);

	rx_total = uart_emul_get_tx_data(uart_sink_dev, rx_buf, sizeof(rx_buf));
	zassert_equal(rx_total, sizeof(tx_data),
		      "stress: expected %u, got %u",
		      (unsigned)sizeof(tx_data), rx_total);
	zassert_mem_equal(rx_buf, tx_data, sizeof(tx_data));
}

/* ------------------------------------------------------------------ */
/* Megabyte-scale stress tests                                         */
/* ------------------------------------------------------------------ */

/*
 * Generate a deterministic byte at a given position so we can verify
 * the output without keeping the entire input in memory.
 */
static inline uint8_t pattern_byte(uint32_t pos)
{
	return (uint8_t)((pos * 131u + 17u) & 0xffu);
}

/*
 * Pump @a total bytes through the pipeline, feeding data in @a chunk
 * sized pieces and continuously draining the sink side.  Verifies
 * every single received byte against the expected pattern.
 */
static void run_large_transfer(uint32_t total, uint32_t chunk)
{
	/* Reusable I/O buffers — kept static so they don't blow the stack. */
	static uint8_t tx_buf[8192];
	static uint8_t rx_buf[8192];
	uint32_t tx_pos = 0;
	uint32_t rx_pos = 0;
	int ret;

	ret = zstnode_start(src_dev);
	zassert_equal(ret, 0, "src start");

	while (tx_pos < total || rx_pos < total) {
		/* --- TX side: push the next chunk into the source UART --- */
		if (tx_pos < total) {
			uint32_t want = chunk;

			if (want > sizeof(tx_buf)) {
				want = sizeof(tx_buf);
			}
			if (want > total - tx_pos) {
				want = total - tx_pos;
			}

			for (uint32_t i = 0; i < want; i++) {
				tx_buf[i] = pattern_byte(tx_pos + i);
			}

			uint32_t sent = uart_emul_put_rx_data(
				uart_src_dev, tx_buf, want);
			tx_pos += sent;
		}

		/* Let the pipeline move data. */
		k_msleep(1);

		/* --- RX side: drain whatever the sink has produced --- */
		for (;;) {
			uint32_t want = sizeof(rx_buf);

			if (want > total - rx_pos) {
				want = total - rx_pos;
			}
			if (want == 0) {
				break;
			}

			uint32_t got = uart_emul_get_tx_data(
				uart_sink_dev, rx_buf, want);
			if (got == 0) {
				break;
			}

			/* Verify every byte. */
			for (uint32_t i = 0; i < got; i++) {
				uint8_t exp = pattern_byte(rx_pos + i);

				zassert_equal(
					rx_buf[i], exp,
					"mismatch at byte %u: 0x%02x != 0x%02x",
					rx_pos + i, rx_buf[i], exp);
			}
			rx_pos += got;
		}
	}

	ret = zstnode_stop(src_dev);
	zassert_equal(ret, 0, "src stop");

	zassert_equal(rx_pos, total,
		      "total mismatch: expected %u, got %u", total, rx_pos);
}

ZTEST(zstnode_uart, test_1mb_transfer)
{
	/* 1 MiB streamed in 4 KiB chunks — every byte verified. */
	run_large_transfer(1024u * 1024u, 4096u);
}

ZTEST(zstnode_uart, test_2mb_varied_chunks)
{
	/*
	 * 2 MiB with alternating small and large chunks to stress
	 * buffer boundary handling.
	 */
	static uint8_t tx_buf[8192];
	static uint8_t rx_buf[8192];
	const uint32_t total = 2u * 1024u * 1024u;
	const uint32_t chunks[] = {137, 4096, 1, 8192, 53, 2048, 7, 512};
	uint32_t tx_pos = 0;
	uint32_t rx_pos = 0;
	uint32_t ci = 0;
	int ret;

	ret = zstnode_start(src_dev);
	zassert_equal(ret, 0);

	while (tx_pos < total || rx_pos < total) {
		if (tx_pos < total) {
			uint32_t want = chunks[ci % ARRAY_SIZE(chunks)];

			ci++;
			if (want > sizeof(tx_buf)) {
				want = sizeof(tx_buf);
			}
			if (want > total - tx_pos) {
				want = total - tx_pos;
			}

			for (uint32_t i = 0; i < want; i++) {
				tx_buf[i] = pattern_byte(tx_pos + i);
			}

			uint32_t sent = uart_emul_put_rx_data(
				uart_src_dev, tx_buf, want);
			tx_pos += sent;
		}

		k_msleep(1);

		for (;;) {
			uint32_t want = sizeof(rx_buf);

			if (want > total - rx_pos) {
				want = total - rx_pos;
			}
			if (want == 0) {
				break;
			}

			uint32_t got = uart_emul_get_tx_data(
				uart_sink_dev, rx_buf, want);
			if (got == 0) {
				break;
			}

			for (uint32_t i = 0; i < got; i++) {
				uint8_t exp = pattern_byte(rx_pos + i);

				zassert_equal(
					rx_buf[i], exp,
					"mismatch at byte %u: 0x%02x != 0x%02x",
					rx_pos + i, rx_buf[i], exp);
			}
			rx_pos += got;
		}
	}

	ret = zstnode_stop(src_dev);
	zassert_equal(ret, 0);

	zassert_equal(rx_pos, total,
		      "total: expected %u, got %u", total, rx_pos);
}

/* ================================================================== */
/* numgen + fakesink test suite                                        */
/* ================================================================== */

static void numgen_cleanup(void *fixture)
{
	ARG_UNUSED(fixture);

	zstnode_stop(numgen_src_dev);
}

ZTEST_SUITE(zstnode_test, NULL, NULL, numgen_cleanup, numgen_cleanup, NULL);

ZTEST(zstnode_test, test_numgen_fakesink_devices_ready)
{
	zassert_true(device_is_ready(numgen_src_dev), "numgen src not ready");
	zassert_true(device_is_ready(fakesink_dev), "fakesink not ready");
}

ZTEST(zstnode_test, test_numgen_fakesink_start_stop)
{
	int ret;

	/* Fakesink start/stop must return -ENOTSUP (non-source). */
	ret = zstnode_start(fakesink_dev);
	zassert_equal(ret, -ENOTSUP, "fakesink start: %d", ret);

	ret = zstnode_stop(fakesink_dev);
	zassert_equal(ret, -ENOTSUP, "fakesink stop: %d", ret);

	/* Source start/stop. */
	ret = zstnode_start(numgen_src_dev);
	zassert_equal(ret, 0, "numgen start failed: %d", ret);

	/* Starting again must return -EALREADY. */
	ret = zstnode_start(numgen_src_dev);
	zassert_equal(ret, -EALREADY, "double start: %d", ret);

	ret = zstnode_stop(numgen_src_dev);
	zassert_equal(ret, 0, "numgen stop failed: %d", ret);

	/* Stopping again must return -EALREADY. */
	ret = zstnode_stop(numgen_src_dev);
	zassert_equal(ret, -EALREADY, "double stop: %d", ret);
}

ZTEST(zstnode_test, test_numgen_fakesink_restart_cycle)
{
	for (int cycle = 0; cycle < 5; cycle++) {
		int ret;

		ret = zstnode_start(numgen_src_dev);
		zassert_equal(ret, 0, "cycle %d numgen start", cycle);

		/* Let the pipeline run briefly. */
		k_msleep(50);

		ret = zstnode_stop(numgen_src_dev);
		zassert_equal(ret, 0, "cycle %d numgen stop", cycle);
	}
}
