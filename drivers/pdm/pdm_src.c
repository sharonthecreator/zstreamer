/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 *
 * ADF-based PDM microphone source with hardware CIC decimation.
 *
 * Configures the STM32U5 Audio Digital Filter (ADF1) to receive a PDM
 * bitstream via its serial interface (SITF0) on ADF1_SDI0, clock the
 * microphone from ADF1_CCK0, and decimate with a hardware Sinc4 CIC
 * filter (DFLT0).  GPDMA transfers decimated 16-bit PCM samples from
 * the upper half of the DFLT0 data register in circular mode to a
 * double buffer.  The source thread wakes on half-transfer /
 * transfer-complete and forwards the buffer into the zstreamer pipeline
 * with zero CPU math.
 */

#include <soc.h>

#include <zephyr/cache.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control/stm32_clock_control.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/dma/dma_stm32.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zstreamer/source.h>

LOG_MODULE_REGISTER(pdm_src, CONFIG_ZSTREAMER_LOG_LEVEL);

#define DT_DRV_COMPAT zstreamer_pdm_src

/* Number of PCM samples per half-buffer.  Each sample is int16_t.
 * Half-buffer byte size equals the graph buffer-size so that one
 * process() call fills exactly one net_buf.
 */
#define HALF_PCM_SAMPLES(inst)                                                 \
  (DT_PROP(DT_PARENT(DT_DRV_INST(inst)), buffer_size) / sizeof(int16_t))

/* Total int16 samples in the double buffer (two halves). */
#define TOTAL_PCM_SAMPLES(inst) (HALF_PCM_SAMPLES(inst) * 2)

/* Decimation ratio = PDM clock / PCM sample rate. */
#define DECIMATION_RATIO(inst)                                                 \
  (DT_INST_PROP(inst, pdm_clk_freq_hz) / DT_INST_PROP(inst, sample_rate_hz))

/* ── Structures ──────────────────────────────────────────────────── */

struct pdm_src_config {
  struct zstreamer_source_config common;
  struct stm32_pclken adf_pclken;
  struct stm32_pclken adf_pclken_src;
  const struct device *dma_dev;
  uint32_t dma_channel;
  uint32_t dma_slot;
  uint32_t sample_rate_hz;
  uint32_t pdm_clk_freq_hz;
  bool right_channel;
  uint16_t half_pcm_samples;
  uint16_t decimation_ratio;
  uint16_t warmup_buffers;
  const struct pinctrl_dev_config *pcfg;
};

struct pdm_src_data {
  struct zstreamer_source_data common;
  struct k_sem half_ready;
  volatile uint8_t ready_half;
  int16_t *dma_buf;
  struct dma_config dma_cfg;
  struct dma_block_config dma_blk;
  uint16_t warmup_remaining;
};

/* ── DMA callback (ISR context) ──────────────────────────────────── */

static void pdm_src_dma_cb(const struct device *dma_dev, void *user_data,
                           uint32_t channel, int status) {
  struct pdm_src_data *data = user_data;

  if (status == DMA_STATUS_BLOCK) {
    data->ready_half = 0;
    k_sem_give(&data->half_ready);
  } else if (status == DMA_STATUS_COMPLETE) {
    data->ready_half = 1;
    k_sem_give(&data->half_ready);
  } else {
    LOG_WRN("DMA error status: %d", status);
  }
}

/* ── Source process (called in source thread loop) ────────────────── */

static int pdm_src_process(const struct device *dev, struct net_buf *buf) {
  const struct pdm_src_config *cfg = dev->config;
  struct pdm_src_data *data = dev->data;

  if (k_sem_take(&data->half_ready, K_MSEC(2000)) != 0) {
    LOG_WRN("PDM DMA half-buffer timeout");
    return -EAGAIN;
  }

  /* Discard initial buffers while the PDM microphone settles. */
  if (data->warmup_remaining > 0) {
    data->warmup_remaining--;
    return -EAGAIN;
  }

  int16_t *src = &data->dma_buf[data->ready_half * cfg->half_pcm_samples];
  size_t half_bytes = cfg->half_pcm_samples * sizeof(int16_t);

  sys_cache_data_invd_range(src, half_bytes);

  /* DMA already produced int16 PCM — just copy into net_buf. */
  net_buf_add_mem(buf, src, half_bytes);

  return 0;
}

/* ── ADF clock setup ─────────────────────────────────────────────── */

static int pdm_src_clock_init(const struct pdm_src_config *cfg,
                              uint32_t *adf_ker_clk) {
  const struct device *clk = DEVICE_DT_GET(STM32_CLOCK_CONTROL_NODE);
  int ret;

  /* Enable ADF1 bus clock on AHB3. */
  ret = clock_control_on(clk, (clock_control_subsys_t)&cfg->adf_pclken);
  if (ret < 0) {
    LOG_ERR("Failed to enable ADF1 bus clock: %d", ret);
    return ret;
  }

  /* Select ADF1 kernel clock source. */
  ret = clock_control_configure(
      clk, (clock_control_subsys_t)&cfg->adf_pclken_src, NULL);
  if (ret < 0) {
    LOG_ERR("Failed to select ADF1 kernel clock: %d", ret);
    return ret;
  }

  /* Read back the actual kernel clock rate. */
  ret = clock_control_get_rate(
      clk, (clock_control_subsys_t)&cfg->adf_pclken_src, adf_ker_clk);
  if (ret < 0) {
    LOG_ERR("Failed to get ADF1 kernel clock rate: %d", ret);
    return ret;
  }

  return 0;
}

/* ── ADF hardware register setup ─────────────────────────────────── */

static int pdm_src_adf_init(const struct pdm_src_config *cfg,
                            uint32_t adf_ker_clk) {
  MDF_TypeDef *adf = ADF1;
  MDF_Filter_TypeDef *flt = ADF1_Filter0;

  /* ── Disable filter and serial interface before configuration ─── */
  flt->DFLTCR &= ~MDF_DFLTCR_DFLTEN;
  flt->SITFCR &= ~MDF_SITFCR_SITFEN;

  /* ── Clock generator ────────────────────────────────────────────
   * PDM_CLK = adf_ker_clk / ((PROCDIV+1) * (CCKDIV+1))
   *
   * We want pdm_clk_freq_hz on CCK0.  Compute the total divider
   * and factor it into PROCDIV and CCKDIV (PROCDIV max 127,
   * CCKDIV max 15).
   */
  uint32_t total_div = adf_ker_clk / cfg->pdm_clk_freq_hz;
  uint32_t procdiv, cckdiv;

  if (total_div == 0) {
    LOG_ERR("ADF kernel clock %u too low for pdm_clk %u", adf_ker_clk,
            cfg->pdm_clk_freq_hz);
    return -EINVAL;
  }

  /* LF_SPI mode requires proc_clk >= 2 * CCK, so CCKDIV >= 1.
   * Factor total_div = (procdiv+1) * (cckdiv+1) with cckdiv >= 1.
   */
  cckdiv = 1;
  while (cckdiv <= 15) {
    if (total_div % (cckdiv + 1) == 0) {
      procdiv = total_div / (cckdiv + 1) - 1;
      if (procdiv <= 127) {
        break;
      }
    }
    cckdiv++;
  }
  if (cckdiv > 15) {
    LOG_ERR("Cannot reach pdm_clk %u from ker_clk %u", cfg->pdm_clk_freq_hz,
            adf_ker_clk);
    return -EINVAL;
  }

  uint32_t actual_pdm_clk = adf_ker_clk / ((procdiv + 1) * (cckdiv + 1));

  /* CKGCR must be configured in two steps (per HAL reference):
   * 1. Write dividers + CCK0 output config with CKDEN=0
   * 2. Set CKDEN to activate the clock generator
   */
  adf->CKGCR = 0U;
  adf->CKGCR = MDF_CKGCR_CCK0EN | MDF_CKGCR_CCK0DIR |
               (cckdiv << MDF_CKGCR_CCKDIV_Pos) |
               (procdiv << MDF_CKGCR_PROCDIV_Pos);
  __DSB();
  adf->CKGCR |= MDF_CKGCR_CKDEN;
  __DSB();

  /* Wait for clock generator to become active. */
  uint32_t timeout = 100000;
  while (!(adf->CKGCR & MDF_CKGCR_CCKACTIVE) && timeout > 0) {
    timeout--;
  }
  if (timeout == 0) {
    LOG_ERR("ADF clock generator did not become active (CKGCR=0x%08x)",
            adf->CKGCR);
    return -ETIMEDOUT;
  }

  /* ── Serial interface (SITF0) ───────────────────────────────────
   * LF_SPI mode (SITFMOD=00), clock from CCK0 (SCKSRC=00).
   * LF_SPI only requires proc_clk >= 2 * CCK (per AN5795).
   */
  flt->SITFCR = 0U; /* LF_SPI mode, CCK0 source */

  /* ── Bitstream matrix ───────────────────────────────────────────
   * Route SITF0 to DFLT0.  BSSEL selects the bitstream edge:
   *   0 = rising-edge data  (right channel, mic Select = VDD)
   *   1 = falling-edge data (left channel, mic Select = GND)
   */
  flt->BSMXCR = cfg->right_channel ? 0U : 1U;

  /* ── CIC filter configuration ───────────────────────────────────
   * Sinc4 mode (CICMOD=100), data from BSMX (DATSRC=00).
   * Decimation ratio = pdm_clk / sample_rate, register = ratio - 1.
   *
   * SCALE must attenuate CIC output to fit 24-bit DFLTDR.
   * CIC max bits = N * log2(R).  For Sinc4 R=125: 4*log2(125) ≈ 28.
   * Need SCALE = -(28 - 24) = -4.  HAL encoding: (-4 - 16) & 0x3F = 44.
   */
  uint32_t mcicd = cfg->decimation_ratio - 1;
  uint32_t scale_reg = (uint32_t)(-4 - 16) & 0x3FU;
  flt->DFLTCICR = MDF_DFLTCICR_CICMOD_2 | /* Sinc4 */
                  (mcicd << MDF_DFLTCICR_MCICD_Pos) |
                  (scale_reg << MDF_DFLTCICR_SCALE_Pos);

  /* Bypass reshape filter; enable high-pass filter to remove DC offset. */
  flt->DFLTRSFR = MDF_DFLTRSFR_RSFLTBYP;

  /* ── Digital filter control ─────────────────────────────────────
   * Enable DMA requests but do NOT enable the filter yet.
   * DFLTEN is set later in pdm_src_hw_start() after DMA is running,
   * to avoid FIFO overflow from DMA requests before the channel is ready.
   */
  flt->DFLTCR = MDF_DFLTCR_DMAEN;

  /* ── Enable serial interface ────────────────────────────────────  */
  flt->SITFCR |= MDF_SITFCR_SITFEN;

  LOG_INF("ADF PDM: ker_clk=%u procdiv=%u cckdiv=%u -> pdm_clk=%u Hz "
          "(target %u), decimation=%u",
          adf_ker_clk, procdiv, cckdiv, actual_pdm_clk, cfg->pdm_clk_freq_hz,
          cfg->decimation_ratio);

  return 0;
}

/* ── DMA setup ───────────────────────────────────────────────────── */

static int pdm_src_dma_init(const struct device *dev) {
  const struct pdm_src_config *cfg = dev->config;
  struct pdm_src_data *data = dev->data;

  /* DMA reads from the upper 16 bits of DFLTDR (MSB-only mode).
   * DFLTDR bits [31:8] hold 24-bit signed data; reading at offset +2
   * yields the upper 16 bits as a signed int16, which is our PCM
   * sample with no further conversion needed.
   */
  uint32_t dfltdr_msb_addr = (uint32_t)&ADF1_Filter0->DFLTDR + 2U;

  memset(&data->dma_blk, 0, sizeof(data->dma_blk));
  data->dma_blk.source_address = dfltdr_msb_addr;
  data->dma_blk.dest_address = (uint32_t)data->dma_buf;
  data->dma_blk.block_size = cfg->half_pcm_samples * 2 * sizeof(int16_t);
  data->dma_blk.source_addr_adj = DMA_ADDR_ADJ_NO_CHANGE;
  data->dma_blk.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT;
  data->dma_blk.source_reload_en = 1;
  data->dma_blk.dest_reload_en = 1;

  memset(&data->dma_cfg, 0, sizeof(data->dma_cfg));
  data->dma_cfg.dma_slot = cfg->dma_slot;
  data->dma_cfg.channel_direction = PERIPHERAL_TO_MEMORY;
  data->dma_cfg.source_data_size = 2; /* 16-bit */
  data->dma_cfg.dest_data_size = 2;
  data->dma_cfg.source_burst_length = 2;
  data->dma_cfg.dest_burst_length = 2;
  data->dma_cfg.dma_callback = pdm_src_dma_cb;
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

/* ── Start: DMA → ADF enable ─────────────────────────────────────── */

static int pdm_src_hw_start(const struct device *dev) {
  const struct pdm_src_config *cfg = dev->config;

  int ret = dma_start(cfg->dma_dev, cfg->dma_channel);
  if (ret < 0) {
    LOG_ERR("DMA start failed: %d", ret);
    return ret;
  }

  /* Now that DMA is running, enable the digital filter. */
  ADF1_Filter0->DFLTCR |= MDF_DFLTCR_DFLTEN;

  LOG_INF("PDM source started (ADF1 hw CIC, decimation=%u)",
          cfg->decimation_ratio);
  return 0;
}

/* ── Device init ─────────────────────────────────────────────────── */

static int pdm_src_init(const struct device *dev) {
  const struct pdm_src_config *cfg = dev->config;
  struct pdm_src_data *data = dev->data;
  int ret;

  k_sem_init(&data->half_ready, 0, 1);
  data->warmup_remaining = cfg->warmup_buffers;

  ret = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
  if (ret < 0) {
    LOG_ERR("Failed to apply pinctrl: %d", ret);
    return ret;
  }

  uint32_t adf_ker_clk;

  ret = pdm_src_clock_init(cfg, &adf_ker_clk);
  if (ret < 0) {
    return ret;
  }

  ret = pdm_src_adf_init(cfg, adf_ker_clk);
  if (ret < 0) {
    return ret;
  }

  ret = pdm_src_dma_init(dev);
  if (ret < 0) {
    return ret;
  }

  ret = pdm_src_hw_start(dev);
  if (ret < 0) {
    return ret;
  }

  return zstreamer_source_common_init(dev);
}

static const struct zstreamer_node_driver_api pdm_src_api = {
    .process = pdm_src_process,
};

/* ── Instance macros ─────────────────────────────────────────────── */

/* ADF1 bus clock: AHB3, bit 10 (RCC_AHB3ENR_ADF1EN). */
#define ADF_PCLKEN(inst)                                                       \
  {                                                                            \
      .bus = STM32_CLOCK_BUS_AHB3,                                             \
      .enr = RCC_AHB3ENR_ADF1EN,                                               \
  }

/* ADF1 kernel clock source: read from the second ``clocks`` entry in
 * the devicetree node.  The overlay must supply two clock cells:
 *   clocks = <&rcc STM32_CLOCK(AHB3, 10)>,
 *            <&rcc STM32_SRC_xxx ADF1_SEL(n)>;
 */
#define ADF_PCLKEN_SRC(inst)                                                   \
  {                                                                            \
      .bus = DT_CLOCKS_CELL_BY_IDX(DT_DRV_INST(inst), 1, bus),                 \
      .enr = DT_CLOCKS_CELL_BY_IDX(DT_DRV_INST(inst), 1, bits),                \
  }

#define PDM_SRC_DEFINE(inst)                                                   \
  BUILD_ASSERT(                                                                \
      DT_INST_PROP(inst, pdm_clk_freq_hz) %                                    \
              DT_INST_PROP(inst, sample_rate_hz) ==                            \
          0,                                                                   \
      "pdm-clk-freq-hz must be an integer multiple of sample-rate-hz");        \
                                                                               \
  ZSTREAMER_SOURCE_DT_INST_PRE_DEFINE(inst);                                   \
  PINCTRL_DT_INST_DEFINE(inst);                                                \
                                                                               \
  static int16_t pdm_dma_buf_##inst[TOTAL_PCM_SAMPLES(inst)] __aligned(32)     \
      __attribute__((section(".noinit")));                                     \
                                                                               \
  static struct pdm_src_data pdm_src_data_##inst = {                           \
      .common = ZSTREAMER_SOURCE_DATA_INIT(inst),                              \
      .dma_buf = pdm_dma_buf_##inst,                                           \
  };                                                                           \
                                                                               \
  static const struct pdm_src_config pdm_src_config_##inst = {                 \
      .common = ZSTREAMER_SOURCE_CONFIG_INIT(inst),                            \
      .adf_pclken = ADF_PCLKEN(inst),                                          \
      .adf_pclken_src = ADF_PCLKEN_SRC(inst),                                  \
      .dma_dev = DEVICE_DT_GET(DT_INST_DMAS_CTLR_BY_NAME(inst, rx)),           \
      .dma_channel = DT_INST_DMAS_CELL_BY_NAME(inst, rx, channel),             \
      .dma_slot = DT_INST_DMAS_CELL_BY_NAME(inst, rx, slot),                   \
      .sample_rate_hz = DT_INST_PROP(inst, sample_rate_hz),                    \
      .pdm_clk_freq_hz = DT_INST_PROP(inst, pdm_clk_freq_hz),                  \
      .right_channel = DT_INST_PROP(inst, right_channel),                      \
      .decimation_ratio = DECIMATION_RATIO(inst),                              \
      .half_pcm_samples = HALF_PCM_SAMPLES(inst),                              \
      .warmup_buffers = DT_INST_PROP(inst, warmup_buffers),                    \
      .pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(inst),                            \
  };                                                                           \
                                                                               \
  DEVICE_DT_INST_DEFINE(inst, pdm_src_init, NULL, &pdm_src_data_##inst,        \
                        &pdm_src_config_##inst, POST_KERNEL,                   \
                        CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &pdm_src_api);

DT_INST_FOREACH_STATUS_OKAY(PDM_SRC_DEFINE)
