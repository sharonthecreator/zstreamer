/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 *
 * STM32-specific ADC source implementation.
 *
 * This implements timer-triggered ADC sampling with DMA for continuous
 * capture at precise sample rates. Designed for audio/signal acquisition.
 *
 * Architecture:
 *   Timer (TRGO) -> ADC (External Trigger) -> DMA (Circular) -> Callback
 *
 * The timer generates TRGO events at the sample rate. Each TRGO triggers
 * an ADC conversion. DMA transfers results to a circular buffer. Half-
 * transfer and transfer-complete interrupts notify when data is ready.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/clock_control/stm32_clock_control.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/irq.h>

/* STM32 LL includes */
#include <stm32_ll_adc.h>
#include <stm32_ll_tim.h>
#include <stm32_ll_dma.h>
#include <stm32_ll_bus.h>

#include "zstsrc_adc_stm32.h"

LOG_MODULE_REGISTER(zstsrc_adc_stm32, CONFIG_ZSTNODE_LOG_LEVEL);

/*
 * ============================================================================
 * STM32U5 SPECIFIC DEFINITIONS
 * ============================================================================
 * These mappings are specific to STM32U5 series. Other STM32 families have
 * different timer-to-ADC trigger connections.
 */

#if defined(CONFIG_SOC_SERIES_STM32U5X)

/*
 * Timer to ADC EXTSEL mapping for STM32U5.
 * Reference: RM0456 Table 186 "ADC external triggers"
 *
 * Note: ADC4 on STM32U5 has a different trigger mapping than ADC1/2.
 * This implementation focuses on ADC1/2.
 */
struct timer_extsel_map {
	TIM_TypeDef *timer;
	uint32_t extsel;
};

static const struct timer_extsel_map timer_extsel_adc12[] = {
	{ TIM1,  LL_ADC_REG_TRIG_EXT_TIM1_TRGO },
	{ TIM2,  LL_ADC_REG_TRIG_EXT_TIM2_TRGO },
	{ TIM3,  LL_ADC_REG_TRIG_EXT_TIM3_TRGO },
	{ TIM4,  LL_ADC_REG_TRIG_EXT_TIM4_TRGO },
	{ TIM6,  LL_ADC_REG_TRIG_EXT_TIM6_TRGO },
	{ TIM8,  LL_ADC_REG_TRIG_EXT_TIM8_TRGO },
#if defined(TIM15)
	{ TIM15, LL_ADC_REG_TRIG_EXT_TIM15_TRGO },
#endif
	{ NULL,  0 }
};

/**
 * Get the ADC EXTSEL value for a given timer.
 */
static uint32_t get_timer_extsel(ADC_TypeDef *adc, TIM_TypeDef *timer)
{
	ARG_UNUSED(adc);

	/* For ADC1/2 on STM32U5 */
	for (int i = 0; timer_extsel_adc12[i].timer != NULL; i++) {
		if (timer_extsel_adc12[i].timer == timer) {
			return timer_extsel_adc12[i].extsel;
		}
	}

	return 0; /* Invalid */
}

/**
 * Get the ADC peripheral pointer from a Zephyr device.
 * STM32U5 has ADC1, ADC2, and ADC4.
 */
static ADC_TypeDef *get_adc_instance(const struct device *adc_dev)
{
	/* The ADC base address is typically in the device config */
	/* For now, we'll use a simple name-based lookup */
	const char *name = adc_dev->name;

	if (strstr(name, "adc@4202") != NULL || strstr(name, "adc1") != NULL) {
		return ADC1;
	}
#if defined(ADC2)
	if (strstr(name, "adc2") != NULL) {
		return ADC2;
	}
#endif
#if defined(ADC4)
	if (strstr(name, "adc4") != NULL) {
		return ADC4;
	}
#endif
	return ADC1; /* Default */
}

/**
 * Get the timer peripheral pointer from a base address.
 */
static TIM_TypeDef *get_timer_from_addr(uintptr_t addr)
{
	/* Cast the address directly to TIM_TypeDef pointer */
	return (TIM_TypeDef *)addr;
}

/**
 * Get timer input clock frequency.
 */
static uint32_t get_timer_clock(TIM_TypeDef *timer)
{
	uint32_t apb_clock;

	/* Most timers on STM32U5 are on APB1 or APB2 */
	/* TIM1, TIM8, TIM15, TIM16, TIM17 are on APB2 */
	/* TIM2, TIM3, TIM4, TIM5, TIM6, TIM7 are on APB1 */

	if (timer == TIM1 || timer == TIM8
#if defined(TIM15)
	    || timer == TIM15
#endif
#if defined(TIM16)
	    || timer == TIM16
#endif
#if defined(TIM17)
	    || timer == TIM17
#endif
	) {
		/* APB2 timers - get PCLK2 */
		/* On STM32U5, timer clock = PCLK if APB prescaler = 1, else 2*PCLK */
		apb_clock = SystemCoreClock; /* Simplified - assumes no prescaler */
	} else {
		/* APB1 timers */
		apb_clock = SystemCoreClock;
	}

	return apb_clock;
}

#else /* !CONFIG_SOC_SERIES_STM32U5X */

/* Placeholder for other STM32 series - implement as needed */
#error "ADC source not yet implemented for this STM32 series"

#endif /* CONFIG_SOC_SERIES_STM32U5X */

/*
 * ============================================================================
 * DMA BUFFER MANAGEMENT
 * ============================================================================
 */

#if defined(CONFIG_ZSTNODE_ADC_DMA_BUFFER_NOCACHE)
/* Place buffer in non-cacheable SRAM4 for STM32U5 */
static uint16_t __aligned(4) __attribute__((section(".nocache")))
	dma_buffer_storage[CONFIG_ZSTNODE_ADC_SRC ? 2048 : 1];
static bool dma_buffer_in_use;
#endif

static void *allocate_dma_buffer(size_t size_bytes)
{
#if defined(CONFIG_ZSTNODE_ADC_DMA_BUFFER_NOCACHE)
	if (dma_buffer_in_use || size_bytes > sizeof(dma_buffer_storage)) {
		LOG_ERR("DMA buffer allocation failed: in_use=%d, size=%zu, max=%zu",
			dma_buffer_in_use, size_bytes, sizeof(dma_buffer_storage));
		return NULL;
	}
	dma_buffer_in_use = true;
	return dma_buffer_storage;
#else
	return k_malloc(size_bytes);
#endif
}

static void free_dma_buffer(void *buffer)
{
#if defined(CONFIG_ZSTNODE_ADC_DMA_BUFFER_NOCACHE)
	if (buffer == dma_buffer_storage) {
		dma_buffer_in_use = false;
	}
#else
	k_free(buffer);
#endif
}

/*
 * ============================================================================
 * DMA CALLBACK HANDLING
 * ============================================================================
 */

/* Global pointer for ISR context - only supports one instance currently */
static struct zstsrc_adc_stm32_data *g_active_data;

/**
 * DMA half-transfer and transfer-complete handler.
 * Called from DMA ISR when half or all of the circular buffer is filled.
 * Note: Currently unused - will be enabled when DMA IRQ integration is complete.
 */
static void __maybe_unused adc_dma_callback(bool half_complete)
{
	struct zstsrc_adc_stm32_data *data = g_active_data;

	if (data == NULL || !data->running || data->cfg.callback == NULL) {
		return;
	}

	const void *buffer;
	size_t half_size = data->cfg.buffer_samples * data->cfg.num_channels;

	if (half_complete) {
		/* First half is ready */
		buffer = data->dma_buffer;
	} else {
		/* Second half is ready */
		buffer = (const uint8_t *)data->dma_buffer +
			 (half_size * data->sample_size);
	}

	data->cfg.callback(data->cfg.user_data, buffer,
			   data->cfg.buffer_samples, data->cfg.num_channels);
}

/*
 * ============================================================================
 * TIMER CONFIGURATION
 * ============================================================================
 */

/**
 * Configure timer for TRGO output at specified frequency.
 */
static int configure_timer_trgo(TIM_TypeDef *timer, uint32_t sample_rate_hz)
{
	uint32_t timer_clock = get_timer_clock(timer);
	uint32_t prescaler = 0;
	uint32_t period;

	/* Calculate prescaler and period for desired frequency */
	/* Timer frequency = timer_clock / ((prescaler + 1) * (period + 1)) */
	/* We want: sample_rate_hz = timer_clock / ((prescaler + 1) * (period + 1)) */

	/* Start with no prescaler and calculate period */
	period = (timer_clock / sample_rate_hz) - 1;

	/* If period is too large, increase prescaler */
	while (period > 0xFFFF && prescaler < 0xFFFF) {
		prescaler++;
		period = (timer_clock / ((prescaler + 1) * sample_rate_hz)) - 1;
	}

	if (period > 0xFFFF || period == 0) {
		LOG_ERR("Cannot achieve sample rate %u Hz (clock=%u)",
			sample_rate_hz, timer_clock);
		return -EINVAL;
	}

	LOG_DBG("Timer config: clock=%u, prescaler=%u, period=%u, rate=%u Hz",
		timer_clock, prescaler, period,
		timer_clock / ((prescaler + 1) * (period + 1)));

	/* Disable timer during configuration */
	LL_TIM_DisableCounter(timer);

	/* Configure timer base */
	LL_TIM_SetPrescaler(timer, prescaler);
	LL_TIM_SetAutoReload(timer, period);
	LL_TIM_SetCounterMode(timer, LL_TIM_COUNTERMODE_UP);

	/* Configure TRGO output on update event */
	LL_TIM_SetTriggerOutput(timer, LL_TIM_TRGO_UPDATE);

	/* Enable auto-reload preload */
	LL_TIM_EnableARRPreload(timer);

	/* Generate update event to load values */
	LL_TIM_GenerateEvent_UPDATE(timer);

	return 0;
}

/*
 * ============================================================================
 * ADC CONFIGURATION
 * ============================================================================
 */

/**
 * Configure ADC for external trigger and DMA.
 */
static int configure_adc(struct zstsrc_adc_stm32_data *data,
			 ADC_TypeDef *adc, TIM_TypeDef *timer)
{
	uint32_t extsel;
	uint32_t resolution;

	/* Get external trigger selection for this timer */
	extsel = get_timer_extsel(adc, timer);
	if (extsel == 0) {
		LOG_ERR("Timer not valid as ADC trigger source");
		return -EINVAL;
	}

	/* Map resolution to LL value */
	switch (data->cfg.resolution) {
	case 6:
		resolution = LL_ADC_RESOLUTION_6B;
		data->sample_size = 1;
		break;
	case 8:
		resolution = LL_ADC_RESOLUTION_8B;
		data->sample_size = 1;
		break;
	case 10:
		resolution = LL_ADC_RESOLUTION_10B;
		data->sample_size = 2;
		break;
	case 12:
		resolution = LL_ADC_RESOLUTION_12B;
		data->sample_size = 2;
		break;
	case 14:
		resolution = LL_ADC_RESOLUTION_14B;
		data->sample_size = 2;
		break;
	default:
		LOG_ERR("Unsupported resolution: %u bits", data->cfg.resolution);
		return -EINVAL;
	}

	/* Disable ADC during configuration */
	if (LL_ADC_IsEnabled(adc)) {
		LL_ADC_Disable(adc);
		while (LL_ADC_IsDisableOngoing(adc)) {
			/* Wait for disable to complete */
		}
	}

	/* Wake from deep power down (STM32U5) */
#if defined(CONFIG_SOC_SERIES_STM32U5X)
	LL_ADC_DisableDeepPowerDown(adc);
#endif

	/* Disable internal voltage regulator, then re-enable */
	LL_ADC_DisableInternalRegulator(adc);
	k_busy_wait(10);
	LL_ADC_EnableInternalRegulator(adc);
	k_busy_wait(LL_ADC_DELAY_INTERNAL_REGUL_STAB_US);

	/* Set resolution */
	LL_ADC_SetResolution(adc, resolution);

	/* Configure data alignment (right-aligned) */
	LL_ADC_SetDataAlignment(adc, LL_ADC_DATA_ALIGN_RIGHT);

	/* Configure sequencer length */
	if (data->cfg.num_channels == 1) {
		LL_ADC_REG_SetSequencerLength(adc, LL_ADC_REG_SEQ_SCAN_DISABLE);
	} else {
		LL_ADC_REG_SetSequencerLength(adc, LL_ADC_REG_SEQ_SCAN_ENABLE_2RANKS);
	}

	/* Configure channels in sequence */
#if defined(CONFIG_SOC_SERIES_STM32U5X)
	/* STM32U5: Enable channel preselection first */
	for (int i = 0; i < data->cfg.num_channels; i++) {
		uint32_t channel = __LL_ADC_DECIMAL_NB_TO_CHANNEL(
			data->cfg.adc_channels[i]);
		LL_ADC_SetChannelPreselection(adc, channel);
	}
#endif

	/* Set channel for rank 1 */
	uint32_t channel1 = __LL_ADC_DECIMAL_NB_TO_CHANNEL(data->cfg.adc_channels[0]);
	LL_ADC_REG_SetSequencerRanks(adc, LL_ADC_REG_RANK_1, channel1);
	/*
	 * Sampling time selection.
	 * STM32U5 ADC1/2: LL_ADC_SAMPLINGTIME_xCYCLES (no _5 suffix)
	 * STM32U5 ADC4:   LL_ADC4_SAMPLINGTIME_xCYCLES_5
	 * Other STM32:    LL_ADC_SAMPLINGTIME_xCYCLES_5
	 *
	 * Using 20 cycles provides good balance of speed and accuracy.
	 */
#if defined(CONFIG_SOC_SERIES_STM32U5X)
	LL_ADC_SetChannelSamplingTime(adc, channel1, LL_ADC_SAMPLINGTIME_20CYCLES);
#else
	LL_ADC_SetChannelSamplingTime(adc, channel1, LL_ADC_SAMPLINGTIME_COMMON_1);
#endif

	/* Set channel for rank 2 if stereo */
	if (data->cfg.num_channels == 2) {
		uint32_t channel2 = __LL_ADC_DECIMAL_NB_TO_CHANNEL(
			data->cfg.adc_channels[1]);
		LL_ADC_REG_SetSequencerRanks(adc, LL_ADC_REG_RANK_2, channel2);
#if defined(CONFIG_SOC_SERIES_STM32U5X)
		LL_ADC_SetChannelSamplingTime(adc, channel2,
					      LL_ADC_SAMPLINGTIME_20CYCLES);
#else
		LL_ADC_SetChannelSamplingTime(adc, channel2,
					      LL_ADC_SAMPLINGTIME_COMMON_1);
#endif
	}

	/* Configure external trigger */
	LL_ADC_REG_SetTriggerSource(adc, extsel);
	LL_ADC_REG_SetTriggerEdge(adc, LL_ADC_REG_TRIG_EXT_RISING);

	/* Configure continuous conversion mode - disabled (we use external trigger) */
	LL_ADC_REG_SetContinuousMode(adc, LL_ADC_REG_CONV_SINGLE);

	/* Configure DMA mode */
#if defined(CONFIG_SOC_SERIES_STM32U5X) || defined(CONFIG_SOC_SERIES_STM32H7X)
	LL_ADC_REG_SetDataTransferMode(adc, LL_ADC_REG_DMA_TRANSFER_UNLIMITED);
#else
	LL_ADC_REG_SetDMATransfer(adc, LL_ADC_REG_DMA_TRANSFER_UNLIMITED);
#endif

	/* Calibrate ADC */
#if defined(CONFIG_SOC_SERIES_STM32U5X)
	/* STM32U5 calibration: offset calibration for single-ended mode */
	LL_ADC_StartCalibration(adc, LL_ADC_SINGLE_ENDED);
	while (LL_ADC_IsCalibrationOnGoing(adc)) {
		/* Wait for calibration */
	}
	k_busy_wait(100);
#endif

	/* Enable ADC */
	LL_ADC_Enable(adc);
	while (!LL_ADC_IsActiveFlag_ADRDY(adc)) {
		/* Wait for ADC ready */
	}
	LL_ADC_ClearFlag_ADRDY(adc);

	LOG_DBG("ADC configured: res=%u bits, channels=%u, extsel=0x%08x",
		data->cfg.resolution, data->cfg.num_channels, extsel);

	return 0;
}

/*
 * ============================================================================
 * DMA CONFIGURATION
 * ============================================================================
 * STM32U5 uses GPDMA which is configured differently than older DMA.
 */

#if defined(CONFIG_SOC_SERIES_STM32U5X)

/* GPDMA request numbers for ADC (from reference manual) */
#define GPDMA_REQ_ADC1  0  /* GPDMA request for ADC1 */

/**
 * Configure GPDMA for circular ADC transfers.
 *
 * Note: On STM32U5, we need to use the GPDMA peripheral directly because
 * Zephyr's DMA driver doesn't support all the features we need for
 * continuous ADC capture (circular mode with half-transfer interrupt).
 */
static int configure_dma_stm32u5(struct zstsrc_adc_stm32_data *data,
				 ADC_TypeDef *adc)
{
	/* For simplicity in this implementation, we'll use GPDMA1 Channel 0 */
	/* In a production implementation, this should be configurable via DT */

	uint32_t buffer_size = data->cfg.dma_buffer_samples * data->sample_size;

	/* Enable GPDMA1 clock */
	LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPDMA1);

	/* Disable channel during configuration */
	LL_DMA_DisableChannel(GPDMA1, LL_DMA_CHANNEL_0);

	/* Reset channel configuration */
	LL_DMA_ResetChannel(GPDMA1, LL_DMA_CHANNEL_0);

	/* Configure transfer: ADC DR -> Memory, circular */
	LL_DMA_SetBlkHWRequest(GPDMA1, LL_DMA_CHANNEL_0, LL_DMA_HWREQUEST_SINGLEBURST);

	/* Source: ADC data register, no increment */
	LL_DMA_SetSrcAddress(GPDMA1, LL_DMA_CHANNEL_0,
			     LL_ADC_DMA_GetRegAddr(adc, LL_ADC_DMA_REG_REGULAR_DATA));
	LL_DMA_SetSrcIncMode(GPDMA1, LL_DMA_CHANNEL_0, LL_DMA_SRC_FIXED);

	/* Destination: DMA buffer, increment */
	LL_DMA_SetDestAddress(GPDMA1, LL_DMA_CHANNEL_0, (uint32_t)data->dma_buffer);
	LL_DMA_SetDestIncMode(GPDMA1, LL_DMA_CHANNEL_0, LL_DMA_DEST_INCREMENT);

	/* Data width based on sample size */
	if (data->sample_size == 2) {
		LL_DMA_SetSrcDataWidth(GPDMA1, LL_DMA_CHANNEL_0, LL_DMA_SRC_DATAWIDTH_HALFWORD);
		LL_DMA_SetDestDataWidth(GPDMA1, LL_DMA_CHANNEL_0, LL_DMA_DEST_DATAWIDTH_HALFWORD);
	} else {
		LL_DMA_SetSrcDataWidth(GPDMA1, LL_DMA_CHANNEL_0, LL_DMA_SRC_DATAWIDTH_BYTE);
		LL_DMA_SetDestDataWidth(GPDMA1, LL_DMA_CHANNEL_0, LL_DMA_DEST_DATAWIDTH_BYTE);
	}

	/* Block size = total samples (in data width units) */
	LL_DMA_SetBlkDataLength(GPDMA1, LL_DMA_CHANNEL_0, buffer_size);

	/* Configure for circular mode by using linked list with loop back */
	/* For now, we'll use a simpler approach: re-enable in ISR */

	/* Request selection: ADC1 */
	LL_DMA_SetPeriphRequest(GPDMA1, LL_DMA_CHANNEL_0, GPDMA_REQ_ADC1);

	/*
	 * Note: For now, we're using polling-based buffer completion
	 * detection instead of DMA interrupts to avoid conflicts with
	 * Zephyr's DMA driver. A production implementation should use
	 * Zephyr's DMA API or coordinate with the existing driver.
	 *
	 * TODO: Implement proper DMA callback integration.
	 */
	ARG_UNUSED(adc);

	LOG_DBG("DMA configured: buffer=%p, size=%u bytes",
		data->dma_buffer, buffer_size);

	return 0;
}

/*
 * Note: DMA ISR functionality is disabled in this initial implementation
 * to avoid IRQ conflicts with Zephyr's DMA driver.
 *
 * A production implementation should either:
 * 1. Use Zephyr's DMA API with proper callback registration
 * 2. Use a dedicated GPDMA channel not used by Zephyr
 * 3. Configure DMA before Zephyr's driver initializes
 */

#endif /* CONFIG_SOC_SERIES_STM32U5X */

/*
 * ============================================================================
 * PUBLIC API
 * ============================================================================
 */

int zstsrc_adc_stm32_init(struct zstsrc_adc_stm32_data *data,
			  const struct zstsrc_adc_stm32_config *cfg)
{
	int ret;

	if (data == NULL || cfg == NULL) {
		return -EINVAL;
	}

	if (cfg->adc_dev == NULL || cfg->timer_addr == 0) {
		LOG_ERR("ADC or timer device not specified");
		return -EINVAL;
	}

	if (cfg->num_channels < 1 || cfg->num_channels > 2) {
		LOG_ERR("Invalid channel count: %u (must be 1 or 2)",
			cfg->num_channels);
		return -EINVAL;
	}

	/* Copy configuration */
	memcpy(&data->cfg, cfg, sizeof(data->cfg));
	data->running = false;
	data->errors = 0;

	/* Get hardware instances */
	ADC_TypeDef *adc = get_adc_instance(cfg->adc_dev);
	TIM_TypeDef *timer = get_timer_from_addr(cfg->timer_addr);

	/* Allocate DMA buffer */
	size_t buffer_size = cfg->dma_buffer_samples * 2; /* Max 16-bit samples */
	data->dma_buffer = allocate_dma_buffer(buffer_size);
	if (data->dma_buffer == NULL) {
		LOG_ERR("Failed to allocate DMA buffer (%zu bytes)", buffer_size);
		return -ENOMEM;
	}

	/* Configure timer for TRGO output */
	ret = configure_timer_trgo(timer, cfg->sample_rate_hz);
	if (ret != 0) {
		free_dma_buffer(data->dma_buffer);
		return ret;
	}

	/* Configure ADC for external trigger and DMA */
	ret = configure_adc(data, adc, timer);
	if (ret != 0) {
		free_dma_buffer(data->dma_buffer);
		return ret;
	}

	/* Configure DMA */
#if defined(CONFIG_SOC_SERIES_STM32U5X)
	ret = configure_dma_stm32u5(data, adc);
#else
	ret = -ENOTSUP;
#endif
	if (ret != 0) {
		free_dma_buffer(data->dma_buffer);
		return ret;
	}

	LOG_INF("ADC source initialized: %u Hz, %u-bit, %u ch",
		cfg->sample_rate_hz, cfg->resolution, cfg->num_channels);

	return 0;
}

int zstsrc_adc_stm32_start(struct zstsrc_adc_stm32_data *data)
{
	if (data == NULL || data->running) {
		return -EINVAL;
	}

	ADC_TypeDef *adc = get_adc_instance(data->cfg.adc_dev);
	TIM_TypeDef *timer = get_timer_from_addr(data->cfg.timer_addr);

	/* Set global pointer for ISR */
	g_active_data = data;
	data->running = true;

	/* Enable DMA channel */
#if defined(CONFIG_SOC_SERIES_STM32U5X)
	LL_DMA_EnableChannel(GPDMA1, LL_DMA_CHANNEL_0);
#endif

	/* Start ADC conversion (will wait for external trigger) */
	LL_ADC_REG_StartConversion(adc);

	/* Start timer - this begins the trigger sequence */
	LL_TIM_EnableCounter(timer);

	LOG_INF("ADC capture started");

	return 0;
}

int zstsrc_adc_stm32_stop(struct zstsrc_adc_stm32_data *data)
{
	if (data == NULL || !data->running) {
		return -EINVAL;
	}

	ADC_TypeDef *adc = get_adc_instance(data->cfg.adc_dev);
	TIM_TypeDef *timer = get_timer_from_addr(data->cfg.timer_addr);

	/* Stop timer first */
	LL_TIM_DisableCounter(timer);

	/* Stop ADC */
	LL_ADC_REG_StopConversion(adc);
	while (LL_ADC_REG_IsStopConversionOngoing(adc)) {
		/* Wait for stop */
	}

	/* Disable DMA */
#if defined(CONFIG_SOC_SERIES_STM32U5X)
	LL_DMA_DisableChannel(GPDMA1, LL_DMA_CHANNEL_0);
#endif

	data->running = false;
	g_active_data = NULL;

	LOG_INF("ADC capture stopped (errors: %u)", data->errors);

	return 0;
}

void zstsrc_adc_stm32_deinit(struct zstsrc_adc_stm32_data *data)
{
	if (data == NULL) {
		return;
	}

	if (data->running) {
		zstsrc_adc_stm32_stop(data);
	}

	/* Disable DMA interrupts */
#if defined(CONFIG_SOC_SERIES_STM32U5X)
	irq_disable(GPDMA1_Channel0_IRQn);
	LL_DMA_DisableIT_HT(GPDMA1, LL_DMA_CHANNEL_0);
	LL_DMA_DisableIT_TC(GPDMA1, LL_DMA_CHANNEL_0);
#endif

	/* Free DMA buffer */
	if (data->dma_buffer != NULL) {
		free_dma_buffer(data->dma_buffer);
		data->dma_buffer = NULL;
	}
}
