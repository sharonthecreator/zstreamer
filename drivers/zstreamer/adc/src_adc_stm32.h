/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 *
 * STM32-specific ADC source implementation header.
 *
 * This provides timer-triggered ADC sampling with DMA for continuous
 * audio/signal capture at precise sample rates.
 */

#ifndef ZSTREAMER_NODE_SRC_ADC_STM32_H_
#define ZSTREAMER_NODE_SRC_ADC_STM32_H_

#include <zephyr/device.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback invoked when a buffer half is ready
 *
 * @param user_data User-provided context
 * @param buffer Pointer to the ready samples
 * @param num_samples Number of samples (per channel) in the buffer
 * @param channels Number of channels (1 or 2)
 */
typedef void (*src_adc_stm32_callback_t)(void *user_data,
					   const void *buffer,
					   size_t num_samples,
					   uint8_t channels);

/**
 * @brief ADC capture configuration
 */
struct src_adc_stm32_config {
	/** ADC device (from io-channels) */
	const struct device *adc_dev;

	/** ADC channel numbers */
	uint8_t adc_channels[2];

	/** Number of channels (1 or 2) */
	uint8_t num_channels;

	/** Timer base address for triggering (e.g., TIM6 peripheral address) */
	uintptr_t timer_addr;

	/** Sample rate in Hz */
	uint32_t sample_rate_hz;

	/** ADC resolution in bits */
	uint8_t resolution;

	/** Samples per buffer (per channel) */
	uint16_t buffer_samples;

	/** Total DMA buffer samples (should be 2x buffer_samples) */
	uint16_t dma_buffer_samples;

	/** Callback when buffer half is ready */
	src_adc_stm32_callback_t callback;

	/** User data for callback */
	void *user_data;
};

/**
 * @brief Runtime state for ADC capture
 */
struct src_adc_stm32_data {
	/** Configuration (copied from init) */
	struct src_adc_stm32_config cfg;

	/** DMA buffer (allocated in non-cacheable region) */
	void *dma_buffer;

	/** Size of each sample in bytes */
	uint8_t sample_size;

	/** Running state */
	bool running;

	/** Error count */
	uint32_t errors;
};

/**
 * @brief Initialize STM32 ADC capture
 *
 * Configures the ADC for external trigger from the specified timer,
 * sets up DMA in circular mode, and prepares the timer for TRGO output.
 *
 * @param data Runtime data structure to initialize
 * @param cfg Configuration parameters
 * @return 0 on success, negative errno on failure
 */
int src_adc_stm32_init(struct src_adc_stm32_data *data,
			  const struct src_adc_stm32_config *cfg);

/**
 * @brief Start ADC capture
 *
 * Starts the timer which triggers ADC conversions. DMA transfers
 * samples to the circular buffer and invokes the callback when
 * each half is complete.
 *
 * @param data Runtime data
 * @return 0 on success, negative errno on failure
 */
int src_adc_stm32_start(struct src_adc_stm32_data *data);

/**
 * @brief Stop ADC capture
 *
 * Stops the timer and ADC, disables DMA transfers.
 *
 * @param data Runtime data
 * @return 0 on success, negative errno on failure
 */
int src_adc_stm32_stop(struct src_adc_stm32_data *data);

/**
 * @brief Deinitialize STM32 ADC capture
 *
 * Releases resources allocated during init.
 *
 * @param data Runtime data
 */
void src_adc_stm32_deinit(struct src_adc_stm32_data *data);

#ifdef __cplusplus
}
#endif

#endif /* ZSTREAMER_NODE_SRC_ADC_STM32_H_ */
