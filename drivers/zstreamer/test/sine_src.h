/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZSTREAMER_TEST_SINE_SRC_H_
#define ZSTREAMER_TEST_SINE_SRC_H_

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

void sine_src_reset(const struct device *dev);

#ifdef __cplusplus
}
#endif

#endif /* ZSTREAMER_TEST_SINE_SRC_H_ */
