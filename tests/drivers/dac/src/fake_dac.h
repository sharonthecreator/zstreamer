/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FAKE_DAC_H_
#define FAKE_DAC_H_

#include <stdint.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FAKE_DAC_MAX_RECORDED 512

uint32_t fake_dac_get_write_count(const struct device *dev);
uint32_t fake_dac_get_last_value(const struct device *dev);

/** Number of recorded values (capped at FAKE_DAC_MAX_RECORDED). */
uint32_t fake_dac_get_recorded_count(const struct device *dev);

/** Get the idx-th recorded value.  Returns 0 if idx is out of range. */
uint32_t fake_dac_get_recorded_value(const struct device *dev, uint32_t idx);

/** Get the channel of the idx-th recorded write.  Returns 0xFF if out of range.
 */
uint8_t fake_dac_get_recorded_channel(const struct device *dev, uint32_t idx);

void fake_dac_reset(const struct device *dev);

#ifdef __cplusplus
}
#endif

#endif /* FAKE_DAC_H_ */
