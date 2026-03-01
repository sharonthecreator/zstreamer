/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SPI_TEST_PERIPHERAL_H_
#define SPI_TEST_PERIPHERAL_H_

#include <stddef.h>
#include <stdint.h>
#include <zephyr/device.h>

/**
 * @brief Load data into the emulator's RX buffer.
 *
 * This data will be returned to the SPI master on subsequent reads.
 *
 * @param dev    The test peripheral device.
 * @param data   Data to load.
 * @param len    Number of bytes.
 * @return Number of bytes actually loaded.
 */
uint32_t spi_test_put_rx_data(const struct device *dev, const uint8_t *data,
                              size_t len);

/**
 * @brief Retrieve data that was written by the SPI master.
 *
 * @param dev    The test peripheral device.
 * @param buf    Buffer to fill.
 * @param len    Maximum bytes to retrieve.
 * @return Number of bytes actually retrieved.
 */
uint32_t spi_test_get_tx_data(const struct device *dev, uint8_t *buf,
                              size_t len);

/**
 * @brief Flush (discard) all pending RX data.
 */
void spi_test_flush_rx(const struct device *dev);

/**
 * @brief Flush (discard) all captured TX data.
 */
void spi_test_flush_tx(const struct device *dev);

#endif /* SPI_TEST_PERIPHERAL_H_ */
