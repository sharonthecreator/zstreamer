/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Public API for the count-sink test driver
 *
 * The count-sink is a terminal node that counts received buffers and
 * bytes using atomic counters. It is intended for verifying pipeline
 * throughput and correctness in tests and samples.
 */

#ifndef ZSTREAMER_TEST_COUNT_SINK_H_
#define ZSTREAMER_TEST_COUNT_SINK_H_

#include <stdint.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get the number of buffers received by a count-sink.
 *
 * @param dev Count-sink device.
 * @return Number of buffers received since last reset.
 */
uint32_t count_sink_get_buf_count(const struct device *dev);

/**
 * @brief Get the total number of bytes received by a count-sink.
 *
 * @param dev Count-sink device.
 * @return Number of bytes received since last reset.
 */
uint32_t count_sink_get_byte_count(const struct device *dev);

/**
 * @brief Reset the buffer and byte counters to zero.
 *
 * Must be called only while the upstream source is stopped.
 *
 * @param dev Count-sink device.
 */
void count_sink_reset(const struct device *dev);

#ifdef __cplusplus
}
#endif

#endif /* ZSTREAMER_TEST_COUNT_SINK_H_ */
