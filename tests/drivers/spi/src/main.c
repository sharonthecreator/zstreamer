/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 *
 * SPI zstreamer node driver tests.
 *
 * Tests the spi-src (source) and spi-sink (sink) drivers
 * using the SPI emulator on native_sim. Only the polling path
 * is exercised because the SPI emulator does not support async.
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/net_buf.h>
#include <zephyr/ztest.h>

#include <zstreamer/node.h>
#include <zstreamer/source.h>

#include <zstreamer_test/helpers.h>

#include "spi_test_peripheral.h"

#define GRAPH_NODE      DT_NODELABEL(spi_streaming_graph)
#define SRC_NODE        DT_NODELABEL(spi_source)
#define SINK_NODE       DT_NODELABEL(spi_sinker)
#define SPI_RX_DEV_NODE DT_NODELABEL(spi_test_rx)
#define SPI_TX_DEV_NODE DT_NODELABEL(spi_test_tx)

static const struct device *graph_dev = DEVICE_DT_GET(GRAPH_NODE);
static const struct device *src_dev = DEVICE_DT_GET(SRC_NODE);
static const struct device *sink_dev = DEVICE_DT_GET(SINK_NODE);
static const struct device *spi_rx_dev = DEVICE_DT_GET(SPI_RX_DEV_NODE);
static const struct device *spi_tx_dev = DEVICE_DT_GET(SPI_TX_DEV_NODE);

/* Drain stale data and stop the source between tests. */
static void cleanup(void *fixture)
{
	ARG_UNUSED(fixture);

	zstreamer_source_stop(src_dev);
	spi_test_flush_rx(spi_rx_dev);
	spi_test_flush_tx(spi_tx_dev);
}

ZTEST_SUITE(zstreamer_node_spi, NULL, NULL, cleanup, cleanup, NULL);

/* ------------------------------------------------------------------ */
/* Basic functional tests                                              */
/* ------------------------------------------------------------------ */

ZTEST(zstreamer_node_spi, test_devices_ready)
{
	zassert_true(device_is_ready(graph_dev), "graph not ready");
	zassert_true(device_is_ready(src_dev), "src not ready");
	zassert_true(device_is_ready(sink_dev), "sink not ready");
	zassert_true(device_is_ready(spi_rx_dev), "spi rx dev not ready");
	zassert_true(device_is_ready(spi_tx_dev), "spi tx dev not ready");
}

ZTEST(zstreamer_node_spi, test_start_stop)
{
	int ret;

	/* Source start/stop. */
	ret = zstreamer_source_start(src_dev);
	zassert_equal(ret, 0, "src start failed: %d", ret);

	/* Starting again must return -EALREADY. */
	ret = zstreamer_source_start(src_dev);
	zassert_equal(ret, -EALREADY, "double start: %d", ret);

	ret = zstreamer_source_stop(src_dev);
	zassert_equal(ret, 0, "src stop failed: %d", ret);

	/* Stopping again must return -EALREADY. */
	ret = zstreamer_source_stop(src_dev);
	zassert_equal(ret, -EALREADY, "double stop: %d", ret);
}

ZTEST(zstreamer_node_spi, test_buf_alloc)
{
	struct net_buf *buf;

	buf = zstreamer_node_alloc_buf(src_dev, K_MSEC(100));
	zassert_not_null(buf, "buf alloc failed");
	zassert_true(net_buf_tailroom(buf) > 0, "no tailroom");
	net_buf_unref(buf);
}

ZTEST(zstreamer_node_spi, test_spi_relay)
{
	/*
	 * Pre-load data into the source SPI peripheral emulator.
	 * The source node reads from SPI, the data flows through
	 * the pipeline, and the sink node writes it to the other
	 * SPI peripheral. We then extract what the sink wrote.
	 */
	const uint8_t tx_data[] = "hello spi";
	const size_t tx_len = sizeof(tx_data) - 1;
	uint8_t rx_buf[64];
	uint32_t rx_len;
	int ret;

	/*
	 * Load enough data for the source's rx-length (64 bytes).
	 * The first 9 bytes are our payload; the rest will be zeros
	 * from the emulator's zero-fill when the ring buffer is empty.
	 */
	spi_test_put_rx_data(spi_rx_dev, tx_data, tx_len);

	ret = zstreamer_source_start(src_dev);
	zassert_equal(ret, 0, "src start: %d", ret);

	/* Let the pipeline process data.
	 * The source reads 64-byte frames. Give it time for one cycle.
	 */
	k_msleep(500);

	ret = zstreamer_source_stop(src_dev);
	zassert_equal(ret, 0);

	rx_len = spi_test_get_tx_data(spi_tx_dev, rx_buf, sizeof(rx_buf));
	/*
	 * The source reads 64 bytes per transaction, so the sink
	 * should have written at least 64 bytes. Verify the first
	 * tx_len bytes match our input.
	 */
	zassert_true(rx_len >= tx_len, "expected at least %u bytes, got %u", tx_len, rx_len);
	zassert_mem_equal(rx_buf, tx_data, tx_len, "relayed data mismatch");
}

ZTEST(zstreamer_node_spi, test_spi_long_transfer)
{
	/*
	 * Stream 256 bytes through the SPI pipeline.
	 * The source reads 64 bytes per frame, so this requires
	 * at least 4 read cycles.
	 */
	uint8_t tx_data[256];
	uint8_t rx_buf[300];
	uint32_t rx_total;
	int ret;

	for (size_t i = 0; i < sizeof(tx_data); i++) {
		tx_data[i] = (uint8_t)(i & 0xff);
	}

	spi_test_put_rx_data(spi_rx_dev, tx_data, sizeof(tx_data));

	ret = zstreamer_source_start(src_dev);
	zassert_equal(ret, 0);

	k_msleep(2000);

	ret = zstreamer_source_stop(src_dev);
	zassert_equal(ret, 0);

	rx_total = spi_test_get_tx_data(spi_tx_dev, rx_buf, sizeof(rx_buf));
	/*
	 * We expect at least 256 bytes to have been relayed.
	 * The source may have read extra zero-filled frames beyond
	 * the 256 bytes we loaded, so rx_total >= 256.
	 */
	zassert_true(rx_total >= sizeof(tx_data), "long xfer: expected >= %u, got %u",
		     (unsigned)sizeof(tx_data), rx_total);
	zassert_mem_equal(rx_buf, tx_data, sizeof(tx_data));
}

ZTEST(zstreamer_node_spi, test_spi_restart_cycle)
{
	uint8_t pattern[8] = {0xAA, 0xBB, 0xCC, 0xDD, 0x11, 0x22, 0x33, 0x44};
	uint8_t rx_buf[64];
	uint32_t rx_len;

	for (int cycle = 0; cycle < 5; cycle++) {
		int ret;

		spi_test_flush_rx(spi_rx_dev);
		spi_test_flush_tx(spi_tx_dev);

		spi_test_put_rx_data(spi_rx_dev, pattern, sizeof(pattern));

		ret = zstreamer_source_start(src_dev);
		zassert_equal(ret, 0, "cycle %d src start", cycle);

		k_msleep(300);

		ret = zstreamer_source_stop(src_dev);
		zassert_equal(ret, 0);

		rx_len = spi_test_get_tx_data(spi_tx_dev, rx_buf, sizeof(rx_buf));
		zassert_true(rx_len >= sizeof(pattern), "cycle %d: expected >= %u, got %u", cycle,
			     (unsigned)sizeof(pattern), rx_len);
		zassert_mem_equal(rx_buf, pattern, sizeof(pattern), "cycle %d: data mismatch",
				  cycle);
	}
}

ZTEST(zstreamer_node_spi, test_spi_stress)
{
	/*
	 * Push 1024 bytes through the pipeline.
	 * Each SPI read is 64 bytes, so the source must complete
	 * 16 read transactions.
	 */
	uint8_t tx_data[1024];
	uint8_t rx_buf[1100];
	uint32_t rx_total;
	int ret;

	for (size_t i = 0; i < sizeof(tx_data); i++) {
		tx_data[i] = (uint8_t)((i * 7 + 13) & 0xff);
	}

	spi_test_put_rx_data(spi_rx_dev, tx_data, sizeof(tx_data));

	ret = zstreamer_source_start(src_dev);
	zassert_equal(ret, 0);

	k_msleep(3000);

	ret = zstreamer_source_stop(src_dev);
	zassert_equal(ret, 0);

	rx_total = spi_test_get_tx_data(spi_tx_dev, rx_buf, sizeof(rx_buf));
	zassert_true(rx_total >= sizeof(tx_data), "stress: expected >= %u, got %u",
		     (unsigned)sizeof(tx_data), rx_total);
	zassert_mem_equal(rx_buf, tx_data, sizeof(tx_data));
}

/* ------------------------------------------------------------------ */
/* Pattern verification for large transfers                            */
/* ------------------------------------------------------------------ */

static void run_large_spi_transfer(uint32_t total, uint32_t chunk)
{
	static uint8_t tx_buf[2048];
	static uint8_t rx_buf[2048];
	uint32_t tx_pos = 0;
	uint32_t rx_pos = 0;
	int ret;

	ret = zstreamer_source_start(src_dev);
	zassert_equal(ret, 0, "src start");

	while (tx_pos < total || rx_pos < total) {
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

			tx_pos += spi_test_put_rx_data(spi_rx_dev, tx_buf, want);
		}

		k_msleep(1);

		for (;;) {
			uint32_t want = sizeof(rx_buf);
			uint32_t got;

			if (want > total - rx_pos) {
				want = total - rx_pos;
			}
			if (want == 0) {
				break;
			}

			got = spi_test_get_tx_data(spi_tx_dev, rx_buf, want);
			if (got == 0) {
				break;
			}

			for (uint32_t i = 0; i < got; i++) {
				uint8_t exp = pattern_byte(rx_pos + i);

				zassert_equal(rx_buf[i], exp,
					      "mismatch at byte %u: 0x%02x != 0x%02x", rx_pos + i,
					      rx_buf[i], exp);
			}
			rx_pos += got;
		}
	}

	ret = zstreamer_source_stop(src_dev);
	zassert_equal(ret, 0, "src stop");

	zassert_equal(rx_pos, total, "total mismatch: expected %u, got %u", total, rx_pos);
}

ZTEST(zstreamer_node_spi, test_spi_large_verified)
{
	/*
	 * Stream 4 KiB and verify every byte.
	 * Load the full payload into the source emulator first,
	 * then run the pipeline and verify on the sink side.
	 */
	static uint8_t tx_data[4096];
	static uint8_t rx_data[4200];
	uint32_t rx_total;
	int ret;

	for (size_t i = 0; i < sizeof(tx_data); i++) {
		tx_data[i] = pattern_byte(i);
	}

	spi_test_put_rx_data(spi_rx_dev, tx_data, sizeof(tx_data));

	ret = zstreamer_source_start(src_dev);
	zassert_equal(ret, 0);

	k_msleep(5000);

	ret = zstreamer_source_stop(src_dev);
	zassert_equal(ret, 0);

	rx_total = spi_test_get_tx_data(spi_tx_dev, rx_data, sizeof(rx_data));
	zassert_true(rx_total >= sizeof(tx_data), "large: expected >= %u, got %u",
		     (unsigned)sizeof(tx_data), rx_total);

	for (size_t i = 0; i < sizeof(tx_data); i++) {
		zassert_equal(rx_data[i], tx_data[i], "mismatch at byte %u: 0x%02x != 0x%02x",
			      (unsigned)i, rx_data[i], tx_data[i]);
	}
}

ZTEST(zstreamer_node_spi, test_spi_64kb_streamed)
{
	/* 64 KiB end-to-end with continuous feed and drain. */
	run_large_spi_transfer(64u * 1024u, 1536u);
}

ZTEST(zstreamer_node_spi, test_spi_96kb_varied_chunks)
{
	/* 96 KiB with mixed chunk sizes (all >= 64-byte SPI frame). */
	static const uint16_t chunks[] = {64, 137, 511, 1024, 2048, 777, 1536};
	const uint32_t total = 96u * 1024u;
	uint32_t tx_pos = 0;
	uint32_t rx_pos = 0;
	uint32_t ci = 0;
	static uint8_t tx_buf[2048];
	static uint8_t rx_buf[2048];
	int ret;

	ret = zstreamer_source_start(src_dev);
	zassert_equal(ret, 0, "src start");

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

			tx_pos += spi_test_put_rx_data(spi_rx_dev, tx_buf, want);
		}

		k_msleep(1);

		for (;;) {
			uint32_t want = sizeof(rx_buf);
			uint32_t got;

			if (want > total - rx_pos) {
				want = total - rx_pos;
			}
			if (want == 0) {
				break;
			}

			got = spi_test_get_tx_data(spi_tx_dev, rx_buf, want);
			if (got == 0) {
				break;
			}

			for (uint32_t i = 0; i < got; i++) {
				uint8_t exp = pattern_byte(rx_pos + i);

				zassert_equal(rx_buf[i], exp,
					      "mismatch at byte %u: 0x%02x != 0x%02x", rx_pos + i,
					      rx_buf[i], exp);
			}
			rx_pos += got;
		}
	}

	ret = zstreamer_source_stop(src_dev);
	zassert_equal(ret, 0, "src stop");

	zassert_equal(rx_pos, total, "total mismatch: expected %u, got %u", total, rx_pos);
}
