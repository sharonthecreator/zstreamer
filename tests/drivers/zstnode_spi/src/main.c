/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 *
 * SPI zstnode driver tests.
 *
 * Tests the zstsrc-spi (source) and zstsink-spi (sink) drivers
 * using the SPI emulator on native_sim. Only the polling path
 * is exercised because the SPI emulator does not support async.
 */

#include <string.h>

#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/net_buf.h>

#include <zephyr/drivers/zstnode.h>
#include <zstreamer/zstreamer.h>

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

/* Drain stale data and stop the pipeline between tests. */
static void cleanup(void *fixture)
{
	ARG_UNUSED(fixture);

	zstreamer_stop(src_dev);
	zstreamer_stop(sink_dev);
	spi_test_flush_rx(spi_rx_dev);
	spi_test_flush_tx(spi_tx_dev);
}

ZTEST_SUITE(zstnode_spi, NULL, NULL, cleanup, cleanup, NULL);

/* ------------------------------------------------------------------ */
/* Basic functional tests                                              */
/* ------------------------------------------------------------------ */

ZTEST(zstnode_spi, test_devices_ready)
{
	zassert_true(device_is_ready(graph_dev), "graph not ready");
	zassert_true(device_is_ready(src_dev), "src not ready");
	zassert_true(device_is_ready(sink_dev), "sink not ready");
	zassert_true(device_is_ready(spi_rx_dev), "spi rx dev not ready");
	zassert_true(device_is_ready(spi_tx_dev), "spi tx dev not ready");
}

ZTEST(zstnode_spi, test_start_stop)
{
	int ret;

	ret = zstreamer_start(sink_dev);
	zassert_equal(ret, 0, "sink start failed: %d", ret);

	ret = zstreamer_start(src_dev);
	zassert_equal(ret, 0, "src start failed: %d", ret);

	/* Starting again must return -EALREADY. */
	ret = zstreamer_start(src_dev);
	zassert_equal(ret, -EALREADY, "double start: %d", ret);

	ret = zstreamer_stop(src_dev);
	zassert_equal(ret, 0, "src stop failed: %d", ret);

	ret = zstreamer_stop(sink_dev);
	zassert_equal(ret, 0, "sink stop failed: %d", ret);

	/* Stopping again must return -EALREADY. */
	ret = zstreamer_stop(sink_dev);
	zassert_equal(ret, -EALREADY, "double stop: %d", ret);
}

ZTEST(zstnode_spi, test_buf_alloc)
{
	struct net_buf *buf;

	buf = zstreamer_alloc_buf(src_dev, K_MSEC(100));
	zassert_not_null(buf, "buf alloc failed");
	zassert_true(net_buf_tailroom(buf) > 0, "no tailroom");
	net_buf_unref(buf);
}

ZTEST(zstnode_spi, test_spi_relay)
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

	ret = zstreamer_start(sink_dev);
	zassert_equal(ret, 0, "sink start: %d", ret);

	ret = zstreamer_start(src_dev);
	zassert_equal(ret, 0, "src start: %d", ret);

	/* Let the pipeline process data.
	 * The source reads 64-byte frames. Give it time for one cycle.
	 */
	k_msleep(500);

	ret = zstreamer_stop(src_dev);
	zassert_equal(ret, 0);

	ret = zstreamer_stop(sink_dev);
	zassert_equal(ret, 0);

	rx_len = spi_test_get_tx_data(spi_tx_dev, rx_buf, sizeof(rx_buf));
	/*
	 * The source reads 64 bytes per transaction, so the sink
	 * should have written at least 64 bytes. Verify the first
	 * tx_len bytes match our input.
	 */
	zassert_true(rx_len >= tx_len,
		     "expected at least %u bytes, got %u", tx_len, rx_len);
	zassert_mem_equal(rx_buf, tx_data, tx_len, "relayed data mismatch");
}

ZTEST(zstnode_spi, test_spi_long_transfer)
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

	ret = zstreamer_start(sink_dev);
	zassert_equal(ret, 0);

	ret = zstreamer_start(src_dev);
	zassert_equal(ret, 0);

	k_msleep(2000);

	ret = zstreamer_stop(src_dev);
	zassert_equal(ret, 0);
	ret = zstreamer_stop(sink_dev);
	zassert_equal(ret, 0);

	rx_total = spi_test_get_tx_data(spi_tx_dev, rx_buf, sizeof(rx_buf));
	/*
	 * We expect at least 256 bytes to have been relayed.
	 * The source may have read extra zero-filled frames beyond
	 * the 256 bytes we loaded, so rx_total >= 256.
	 */
	zassert_true(rx_total >= sizeof(tx_data),
		     "long xfer: expected >= %u, got %u",
		     (unsigned)sizeof(tx_data), rx_total);
	zassert_mem_equal(rx_buf, tx_data, sizeof(tx_data));
}

ZTEST(zstnode_spi, test_spi_restart_cycle)
{
	uint8_t pattern[8] = {0xAA, 0xBB, 0xCC, 0xDD, 0x11, 0x22, 0x33, 0x44};
	uint8_t rx_buf[64];
	uint32_t rx_len;

	for (int cycle = 0; cycle < 5; cycle++) {
		int ret;

		spi_test_flush_rx(spi_rx_dev);
		spi_test_flush_tx(spi_tx_dev);

		spi_test_put_rx_data(spi_rx_dev, pattern, sizeof(pattern));

		ret = zstreamer_start(sink_dev);
		zassert_equal(ret, 0, "cycle %d sink start", cycle);
		ret = zstreamer_start(src_dev);
		zassert_equal(ret, 0, "cycle %d src start", cycle);

		k_msleep(300);

		ret = zstreamer_stop(src_dev);
		zassert_equal(ret, 0);
		ret = zstreamer_stop(sink_dev);
		zassert_equal(ret, 0);

		rx_len = spi_test_get_tx_data(spi_tx_dev,
					      rx_buf, sizeof(rx_buf));
		zassert_true(rx_len >= sizeof(pattern),
			     "cycle %d: expected >= %u, got %u",
			     cycle, (unsigned)sizeof(pattern), rx_len);
		zassert_mem_equal(rx_buf, pattern, sizeof(pattern),
				  "cycle %d: data mismatch", cycle);
	}
}

ZTEST(zstnode_spi, test_spi_stress)
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

	ret = zstreamer_start(sink_dev);
	zassert_equal(ret, 0);
	ret = zstreamer_start(src_dev);
	zassert_equal(ret, 0);

	k_msleep(3000);

	ret = zstreamer_stop(src_dev);
	zassert_equal(ret, 0);
	ret = zstreamer_stop(sink_dev);
	zassert_equal(ret, 0);

	rx_total = spi_test_get_tx_data(spi_tx_dev, rx_buf, sizeof(rx_buf));
	zassert_true(rx_total >= sizeof(tx_data),
		     "stress: expected >= %u, got %u",
		     (unsigned)sizeof(tx_data), rx_total);
	zassert_mem_equal(rx_buf, tx_data, sizeof(tx_data));
}

/* ------------------------------------------------------------------ */
/* Pattern verification for large transfers                            */
/* ------------------------------------------------------------------ */

static inline uint8_t pattern_byte(uint32_t pos)
{
	return (uint8_t)((pos * 131u + 17u) & 0xffu);
}

ZTEST(zstnode_spi, test_spi_large_verified)
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

	ret = zstreamer_start(sink_dev);
	zassert_equal(ret, 0);
	ret = zstreamer_start(src_dev);
	zassert_equal(ret, 0);

	k_msleep(5000);

	ret = zstreamer_stop(src_dev);
	zassert_equal(ret, 0);
	ret = zstreamer_stop(sink_dev);
	zassert_equal(ret, 0);

	rx_total = spi_test_get_tx_data(spi_tx_dev, rx_data, sizeof(rx_data));
	zassert_true(rx_total >= sizeof(tx_data),
		     "large: expected >= %u, got %u",
		     (unsigned)sizeof(tx_data), rx_total);

	for (size_t i = 0; i < sizeof(tx_data); i++) {
		zassert_equal(rx_data[i], tx_data[i],
			      "mismatch at byte %u: 0x%02x != 0x%02x",
			      (unsigned)i, rx_data[i], tx_data[i]);
	}
}
