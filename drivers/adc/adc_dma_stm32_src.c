/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 *
 * Timer-triggered STM32 ADC source with circular DMA for continuous
 * capture (e.g. audio).
 *
 * A hardware timer's TRGO triggers ADC conversions at sample-rate-hz.
 * DMA transfers results in circular mode to a double buffer.  The
 * source thread wakes on half-transfer / transfer-complete and copies
 * a half-buffer into the zstreamer pipeline as signed int16 PCM.
 *
 * STM32-only: Zephyr's adc_stm32 driver cannot stream (its DMA path is
 * one-shot per sequence, software-triggered), so this driver uses the
 * Zephyr ADC API for channel setup only and drops to STM32 LL for
 * trigger / unlimited-DMA / sequencer / oversampling configuration.
 * The referenced ADC controller must be status "okay" and is used
 * exclusively by this node — concurrent adc_read() calls on the same
 * instance would clobber the trigger/DMA state.
 */

#define DT_DRV_COMPAT zstreamer_adc_dma_stm32_src

#include <zephyr/cache.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/clock_control/stm32_clock_control.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/dma/dma_stm32.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <stm32_ll_adc.h>
#include <stm32_ll_tim.h>

#include <zstreamer/source.h>

LOG_MODULE_REGISTER(adc_dma_stm32_src, CONFIG_ZSTREAMER_LOG_LEVEL);

/* Number of samples per half-buffer.  Each sample is uint16_t (2 bytes).
 * Half-buffer byte size must equal the graph buffer-size so that one
 * process() call fills exactly one net_buf.
 */
#define HALF_BUF_SAMPLES(inst)                                                                     \
	(DT_PROP(DT_PARENT(DT_DRV_INST(inst)), buffer_size) / sizeof(uint16_t))

#define TOTAL_SAMPLES(inst) (HALF_BUF_SAMPLES(inst) * 2)

/* ADC controller node backing this source (io-channels phandle). */
#define ADC_NODE(inst) DT_INST_IO_CHANNELS_CTLR(inst)

struct adc_dma_stm32_src_config {
	struct zstreamer_source_config common;
	const struct adc_dt_spec adc_spec;
	ADC_TypeDef *adc_base;
	bool seq_fixed;          /* ADC node's st,adc-sequencer == "fixed" */
	uint32_t trigger_source; /* LL_ADC_REG_TRIG_EXT_<st,trigger> */
	uint32_t ovs_ratio;      /* LL_ADC_OVS_RATIO_x, or 0 = disabled */
	const struct device *dma_dev;
	uint32_t dma_channel;
	uint32_t dma_slot;
	const struct stm32_pclken *tim_pclken;
	size_t tim_pclk_len;
	TIM_TypeDef *tim_reg;
	uint32_t sample_rate_hz;
	uint16_t half_buf_samples;
};

struct adc_dma_stm32_src_data {
	struct zstreamer_source_data common;
	struct k_sem half_ready;
	volatile uint8_t ready_half; /* 0 = first half, 1 = second half */
	uint16_t *dma_buf;
	int32_t dc_estimate;
	struct dma_config dma_cfg;
	struct dma_block_config dma_blk;
};

/* ── DMA callback (ISR context) ─────────────────────────────────────── */

static void adc_dma_stm32_src_dma_cb(const struct device *dma_dev, void *user_data,
				     uint32_t channel, int status)
{
	struct adc_dma_stm32_src_data *data = user_data;

	if (status == DMA_STATUS_BLOCK) {
		/* Half-transfer: first half is ready. */
		data->ready_half = 0;
		k_sem_give(&data->half_ready);
	} else if (status == DMA_STATUS_COMPLETE) {
		/* Transfer-complete: second half is ready. */
		data->ready_half = 1;
		k_sem_give(&data->half_ready);
	}
}

/* ── Source process (called in source thread loop) ───────────────── */

static int adc_dma_stm32_src_process(const struct device *dev, struct net_buf *buf)
{
	const struct adc_dma_stm32_src_config *cfg = dev->config;
	struct adc_dma_stm32_src_data *data = dev->data;

	/* Timeout must exceed one half-buffer fill time (half_buf_samples /
	 * sample_rate). */
	if (k_sem_take(&data->half_ready, K_MSEC(2000)) != 0) {
		LOG_WRN("DMA half-buffer timeout");
		return -EAGAIN;
	}

	uint16_t *src = &data->dma_buf[data->ready_half * cfg->half_buf_samples];
	size_t half_bytes = cfg->half_buf_samples * sizeof(uint16_t);

	/* Invalidate cache so CPU sees fresh DMA data. */
	sys_cache_data_invd_range(src, half_bytes);

	int16_t *dst = (int16_t *)net_buf_add(buf, half_bytes);

	for (uint16_t i = 0; i < cfg->half_buf_samples; i++) {
		int32_t sample = (int32_t)src[i];
		data->dc_estimate += (sample - data->dc_estimate) >> 8;
		dst[i] = (int16_t)(sample - data->dc_estimate);
	}

	return 0;
}

/* ── Timer setup ─────────────────────────────────────────────────── */

static int adc_dma_stm32_src_timer_init(const struct adc_dma_stm32_src_config *cfg)
{
	const struct device *clk = DEVICE_DT_GET(STM32_CLOCK_CONTROL_NODE);
	int ret;

	ret = clock_control_on(clk, (clock_control_subsys_t)&cfg->tim_pclken[0]);
	if (ret < 0) {
		LOG_ERR("Failed to enable timer clock: %d", ret);
		return ret;
	}

	/* The timer kernel clock is the second clocks entry (STM32_SRC_TIMPCLK*):
	 * timers run at 2x the APB bus clock whenever the APB prescaler != 1, so
	 * the bus-clock rate (entry 0) is not generally the counting rate. */
	if (cfg->tim_pclk_len < 2) {
		LOG_ERR("Timer node needs a TIMPCLK clocks entry");
		return -EINVAL;
	}

	ret = clock_control_configure(clk, (clock_control_subsys_t)&cfg->tim_pclken[1], NULL);
	if (ret < 0) {
		LOG_ERR("Failed to configure timer kernel clock: %d", ret);
		return ret;
	}

	uint32_t tim_clk;

	ret = clock_control_get_rate(clk, (clock_control_subsys_t)&cfg->tim_pclken[1], &tim_clk);
	if (ret < 0) {
		LOG_ERR("Failed to get timer clock rate: %d", ret);
		return ret;
	}

	uint32_t arr = (tim_clk / cfg->sample_rate_hz) - 1;

	LL_TIM_SetPrescaler(cfg->tim_reg, 0);
	LL_TIM_SetAutoReload(cfg->tim_reg, arr);
	LL_TIM_SetCounterMode(cfg->tim_reg, LL_TIM_COUNTERMODE_UP);
	LL_TIM_SetTriggerOutput(cfg->tim_reg, LL_TIM_TRGO_UPDATE);
	LL_TIM_EnableARRPreload(cfg->tim_reg);
	LL_TIM_GenerateEvent_UPDATE(cfg->tim_reg);

	LOG_INF("TIM: clk=%u arr=%u -> %u Hz", tim_clk, arr, tim_clk / (arr + 1));

	return 0;
}

/* ── DMA setup ───────────────────────────────────────────────────── */

static int adc_dma_stm32_src_dma_init(const struct device *dev)
{
	const struct adc_dma_stm32_src_config *cfg = dev->config;
	struct adc_dma_stm32_src_data *data = dev->data;
	uint32_t adc_dr_addr = LL_ADC_DMA_GetRegAddr(cfg->adc_base, LL_ADC_DMA_REG_REGULAR_DATA);

	memset(&data->dma_blk, 0, sizeof(data->dma_blk));
	data->dma_blk.source_address = adc_dr_addr;
	data->dma_blk.dest_address = (uint32_t)data->dma_buf;
	data->dma_blk.block_size = cfg->half_buf_samples * 2 * sizeof(uint16_t);
	data->dma_blk.source_addr_adj = DMA_ADDR_ADJ_NO_CHANGE;
	data->dma_blk.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT;
	data->dma_blk.source_reload_en = 1;
	data->dma_blk.dest_reload_en = 1;

	memset(&data->dma_cfg, 0, sizeof(data->dma_cfg));
	data->dma_cfg.dma_slot = cfg->dma_slot;
	data->dma_cfg.channel_direction = PERIPHERAL_TO_MEMORY;
	data->dma_cfg.source_data_size = 2; /* 16-bit (driver output contract) */
	data->dma_cfg.dest_data_size = 2;
	data->dma_cfg.source_burst_length = 2;
	data->dma_cfg.dest_burst_length = 2;
	data->dma_cfg.dma_callback = adc_dma_stm32_src_dma_cb;
	data->dma_cfg.user_data = data;
	data->dma_cfg.head_block = &data->dma_blk;
	data->dma_cfg.block_count = 1;
	data->dma_cfg.cyclic = 1;

	int ret = dma_config(cfg->dma_dev, cfg->dma_channel, &data->dma_cfg);

	if (ret < 0) {
		LOG_ERR("DMA config failed: %d", ret);
		return ret;
	}

	return 0;
}

/* ── ADC setup ───────────────────────────────────────────────────── */

static int adc_dma_stm32_src_adc_init(const struct adc_dma_stm32_src_config *cfg)
{
	ADC_TypeDef *adc = cfg->adc_base;
	int ret;

	if (!adc_is_ready_dt(&cfg->adc_spec)) {
		LOG_ERR("ADC device not ready");
		return -ENODEV;
	}

	ret = adc_channel_setup_dt(&cfg->adc_spec);
	if (ret < 0) {
		LOG_ERR("adc_channel_setup failed: %d", ret);
		return ret;
	}

	/* Disable ADC to modify CFGR registers. */
	LL_ADC_Disable(adc);
	while (LL_ADC_IsEnabled(adc)) {
	}

	/* Re-calibrate after reconfiguration for best accuracy. */
#if defined(CONFIG_SOC_SERIES_STM32U5X)
	LL_ADC_StartCalibration(adc, LL_ADC_CALIB_OFFSET);
#else
	LL_ADC_StartCalibration(adc);
#endif
	while (LL_ADC_IsCalibrationOnGoing(adc)) {
	}

	LL_ADC_REG_SetTriggerSource(adc, cfg->trigger_source);

#if defined(CONFIG_SOC_SERIES_STM32U5X)
	/* DMNGT-based series.  CFGR1[1:0] aliases the basic-ADC (ADC4)
	 * DMAEN|DMACFG bits, so this one call covers both instance flavors. */
	LL_ADC_REG_SetDataTransferMode(adc, LL_ADC_REG_DMA_TRANSFER_UNLIMITED);
#else
	LL_ADC_REG_SetDMATransfer(adc, LL_ADC_REG_DMA_TRANSFER_UNLIMITED);
#endif

	LL_ADC_REG_SetContinuousMode(adc, LL_ADC_REG_CONV_SINGLE);
	LL_ADC_REG_SetOverrun(adc, LL_ADC_REG_OVR_DATA_OVERWRITTEN);

	uint32_t channel = __LL_ADC_DECIMAL_NB_TO_CHANNEL(cfg->adc_spec.channel_id);

	if (cfg->seq_fixed) {
		LL_ADC_REG_SetSequencerChannels(adc, channel);
	} else {
		LL_ADC_REG_SetSequencerRanks(adc, LL_ADC_REG_RANK_1, channel);
		LL_ADC_REG_SetSequencerLength(adc, LL_ADC_REG_SEQ_SCAN_DISABLE);
	}

	/* Hardware oversampling, no right-shift: with ratio 16 a 12-bit ADC
	 * yields full-scale 16-bit output.  Note: LL_ADC_OVS_RATIO_x constants
	 * are the basic-ADC flavor — U5's 14-bit ADC1/2 take a plain integer
	 * ratio instead and are not supported here. */
	if (cfg->ovs_ratio != 0) {
		LL_ADC_SetOverSamplingScope(adc, LL_ADC_OVS_GRP_REGULAR_CONTINUED);
		LL_ADC_ConfigOverSamplingRatioShift(adc, cfg->ovs_ratio, LL_ADC_OVS_SHIFT_NONE);
	} else {
		LL_ADC_SetOverSamplingScope(adc, LL_ADC_OVS_DISABLE);
	}

	LL_ADC_Enable(adc);
	while (!LL_ADC_IsActiveFlag_ADRDY(adc)) {
	}

	LOG_INF("ADC ch%u: external trigger, DMA=unlimited, oversampling %s",
		cfg->adc_spec.channel_id, cfg->ovs_ratio != 0 ? "on" : "off");

	return 0;
}

/* ── Start: DMA → ADC → timer ────────────────────────────────────── */

static int adc_dma_stm32_src_hw_start(const struct device *dev)
{
	const struct adc_dma_stm32_src_config *cfg = dev->config;

	int ret = dma_start(cfg->dma_dev, cfg->dma_channel);

	if (ret < 0) {
		LOG_ERR("DMA start failed: %d", ret);
		return ret;
	}

	LL_ADC_REG_StartConversion(cfg->adc_base);
	LL_TIM_EnableCounter(cfg->tim_reg);

	LOG_INF("ADC DMA source started");
	return 0;
}

/* ── Device init ─────────────────────────────────────────────────── */

static int adc_dma_stm32_src_init(const struct device *dev)
{
	const struct adc_dma_stm32_src_config *cfg = dev->config;
	struct adc_dma_stm32_src_data *data = dev->data;
	int ret;

	k_sem_init(&data->half_ready, 0, 1);

	ret = adc_dma_stm32_src_adc_init(cfg);
	if (ret < 0) {
		return ret;
	}

	ret = adc_dma_stm32_src_timer_init(cfg);
	if (ret < 0) {
		return ret;
	}

	ret = adc_dma_stm32_src_dma_init(dev);
	if (ret < 0) {
		return ret;
	}

	ret = adc_dma_stm32_src_hw_start(dev);
	if (ret < 0) {
		return ret;
	}

	return zstreamer_source_common_init(dev);
}

static const struct zstreamer_node_driver_api adc_dma_stm32_src_api = {
	.process = adc_dma_stm32_src_process,
};

/* ── Instance macros ─────────────────────────────────────────────── */

#define TIM_REG(inst) ((TIM_TypeDef *)DT_REG_ADDR(DT_INST_PHANDLE(inst, timer)))

/* st,trigger = "tim6-trgo" -> LL_ADC_REG_TRIG_EXT_TIM6_TRGO.  Pasting the
 * token keeps the series LL header the single source of truth: a trigger
 * that doesn't exist on the target ADC fails to compile. */
#define ADC_TRIG(inst) CONCAT(LL_ADC_REG_TRIG_EXT_, DT_INST_STRING_UPPER_TOKEN(inst, st_trigger))

#define ADC_OVS_RATIO(inst)                                                                        \
	COND_CODE_1(IS_EQ(DT_INST_PROP(inst, oversampling_ratio), 0), (0),           \
              (CONCAT(LL_ADC_OVS_RATIO_,                                       \
                      DT_INST_PROP(inst, oversampling_ratio))))

#define ADC_DMA_STM32_SRC_DEFINE(inst)                                                             \
	BUILD_ASSERT(DT_PROP(DT_PARENT(DT_DRV_INST(inst)), buffer_size) ==                         \
			     HALF_BUF_SAMPLES(inst) * sizeof(uint16_t),                            \
		     "graph buffer-size must equal half_buf_samples * 2");                         \
	/* Halves must not share a cache line on cache-enabled parts, or the                       \
	 * invalidate of one half could discard in-flight DMA of the other. */                     \
	BUILD_ASSERT((HALF_BUF_SAMPLES(inst) * sizeof(uint16_t)) % 32 == 0,                        \
		     "half-buffer size must be a multiple of 32 bytes");                           \
                                                                                                   \
	ZSTREAMER_SOURCE_DT_INST_PRE_DEFINE(inst);                                                 \
                                                                                                   \
	static uint16_t adc_dma_buf_##inst[TOTAL_SAMPLES(inst)] __aligned(32)                      \
	__attribute__((section(".noinit")));                                                       \
                                                                                                   \
	static const struct stm32_pclken tim_pclken_##inst[] =                                     \
		STM32_DT_CLOCKS(DT_INST_PHANDLE(inst, timer));                                     \
                                                                                                   \
	static struct adc_dma_stm32_src_data adc_dma_stm32_src_data_##inst = {                     \
		.common = ZSTREAMER_SOURCE_DATA_INIT(inst),                                        \
		.dma_buf = adc_dma_buf_##inst,                                                     \
		.dc_estimate = 32768,                                                              \
	};                                                                                         \
                                                                                                   \
	static const struct adc_dma_stm32_src_config adc_dma_stm32_src_config_##inst = {           \
		.common = ZSTREAMER_SOURCE_CONFIG_INIT(inst),                                      \
		.adc_spec = ADC_DT_SPEC_GET_BY_IDX(DT_DRV_INST(inst), 0),                          \
		.adc_base = (ADC_TypeDef *)DT_REG_ADDR(ADC_NODE(inst)),                            \
		.seq_fixed = DT_ENUM_HAS_VALUE(ADC_NODE(inst), st_adc_sequencer, fixed),           \
		.trigger_source = ADC_TRIG(inst),                                                  \
		.ovs_ratio = ADC_OVS_RATIO(inst),                                                  \
		.dma_dev = DEVICE_DT_GET(DT_INST_DMAS_CTLR_BY_NAME(inst, rx)),                     \
		.dma_channel = DT_INST_DMAS_CELL_BY_NAME(inst, rx, channel),                       \
		.dma_slot = STM32_DMA_SLOT(inst, rx, slot),                                        \
		.tim_pclken = tim_pclken_##inst,                                                   \
		.tim_pclk_len = ARRAY_SIZE(tim_pclken_##inst),                                     \
		.tim_reg = TIM_REG(inst),                                                          \
		.sample_rate_hz = DT_INST_PROP(inst, sample_rate_hz),                              \
		.half_buf_samples = HALF_BUF_SAMPLES(inst),                                        \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, adc_dma_stm32_src_init, NULL, &adc_dma_stm32_src_data_##inst,  \
			      &adc_dma_stm32_src_config_##inst, POST_KERNEL,                       \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &adc_dma_stm32_src_api);

DT_INST_FOREACH_STATUS_OKAY(ADC_DMA_STM32_SRC_DEFINE)
