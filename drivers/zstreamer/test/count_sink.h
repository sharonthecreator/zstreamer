/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZSTREAMER_TEST_COUNT_SINK_H_
#define ZSTREAMER_TEST_COUNT_SINK_H_

#include <stdint.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t count_sink_get_buf_count(const struct device *dev);
uint32_t count_sink_get_byte_count(const struct device *dev);
void count_sink_reset(const struct device *dev);

#ifdef __cplusplus
}
#endif

#endif /* ZSTREAMER_TEST_COUNT_SINK_H_ */
