/*
 * Copyright (c) 2026 zstreamer contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * SPI test peripheral: a minimal device + emulator for testing
 * zstnode SPI source/sink drivers on native_sim.
 *
 * Provides ring buffers for RX (data returned to master on reads)
 * and TX (data captured from master writes).
 */

#define DT_DRV_COMPAT zstreamer_spi_test_peripheral

#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/spi_emul.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/logging/log.h>

#include "spi_test_peripheral.h"

LOG_MODULE_REGISTER(spi_test_peripheral, LOG_LEVEL_DBG);

#define RING_BUF_SIZE 4096

struct spi_test_data {
	struct ring_buf rx_ring;
	uint8_t rx_buf[RING_BUF_SIZE];
	struct ring_buf tx_ring;
	uint8_t tx_buf[RING_BUF_SIZE];
};

/* ------------------------------------------------------------------ */
/* Public helpers for test code                                        */
/* ------------------------------------------------------------------ */

uint32_t spi_test_put_rx_data(const struct device *dev,
			      const uint8_t *data, size_t len)
{
	struct spi_test_data *ddata = dev->data;

	return ring_buf_put(&ddata->rx_ring, data, len);
}

uint32_t spi_test_get_tx_data(const struct device *dev,
			      uint8_t *buf, size_t len)
{
	struct spi_test_data *ddata = dev->data;

	return ring_buf_get(&ddata->tx_ring, buf, len);
}

void spi_test_flush_rx(const struct device *dev)
{
	struct spi_test_data *ddata = dev->data;

	ring_buf_reset(&ddata->rx_ring);
}

void spi_test_flush_tx(const struct device *dev)
{
	struct spi_test_data *ddata = dev->data;

	ring_buf_reset(&ddata->tx_ring);
}

/* ------------------------------------------------------------------ */
/* Emulator I/O callback                                               */
/* ------------------------------------------------------------------ */

static int spi_test_emul_io(const struct emul *target,
			    const struct spi_config *config,
			    const struct spi_buf_set *tx_bufs,
			    const struct spi_buf_set *rx_bufs)
{
	struct spi_test_data *data = target->data;

	ARG_UNUSED(config);

	/* Capture TX data from master. */
	if (tx_bufs != NULL) {
		for (size_t i = 0; i < tx_bufs->count; i++) {
			const struct spi_buf *b = &tx_bufs->buffers[i];

			if (b->buf != NULL && b->len > 0) {
				ring_buf_put(&data->tx_ring, b->buf, b->len);
			}
		}
	}

	/* Fill RX data for master. */
	if (rx_bufs != NULL) {
		for (size_t i = 0; i < rx_bufs->count; i++) {
			struct spi_buf *b = (struct spi_buf *)&rx_bufs->buffers[i];

			if (b->buf != NULL && b->len > 0) {
				uint32_t got = ring_buf_get(&data->rx_ring,
							    b->buf, b->len);
				/* Zero-fill if not enough data. */
				if (got < b->len) {
					memset((uint8_t *)b->buf + got, 0,
					       b->len - got);
				}
			}
		}
	}

	return 0;
}

static const struct spi_emul_api spi_test_emul_api = {
	.io = spi_test_emul_io,
};

/* ------------------------------------------------------------------ */
/* Device driver (minimal, just makes DEVICE_DT_GET work)             */
/* ------------------------------------------------------------------ */

static int spi_test_dev_init(const struct device *dev)
{
	struct spi_test_data *data = dev->data;

	ring_buf_init(&data->rx_ring, sizeof(data->rx_buf), data->rx_buf);
	ring_buf_init(&data->tx_ring, sizeof(data->tx_buf), data->tx_buf);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Emulator init                                                       */
/* ------------------------------------------------------------------ */

static int spi_test_emul_init(const struct emul *target,
			      const struct device *parent)
{
	ARG_UNUSED(target);
	ARG_UNUSED(parent);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Instantiation                                                       */
/* ------------------------------------------------------------------ */

#define SPI_TEST_PERIPHERAL_DEFINE(inst)                                        \
	static struct spi_test_data spi_test_data_##inst;                      \
	DEVICE_DT_INST_DEFINE(inst, spi_test_dev_init, NULL,                   \
			      &spi_test_data_##inst, NULL,                     \
			      POST_KERNEL,                                     \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, NULL);       \
	EMUL_DT_INST_DEFINE(inst, spi_test_emul_init,                          \
			    &spi_test_data_##inst, NULL,                        \
			    &spi_test_emul_api, NULL);

DT_INST_FOREACH_STATUS_OKAY(SPI_TEST_PERIPHERAL_DEFINE)
