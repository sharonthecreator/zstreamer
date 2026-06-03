/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 *
 * SAI-based PDM microphone source with CIC decimation.
 *
 * Configures STM32 SAI Block A in PDM master-receiver mode.  The SAI
 * generates the PDM bit-clock on CK1 and captures the 1-bit PDM
 * bitstream from D1.  GPDMA transfers raw PDM words in circular mode
 * to a double buffer.  The source thread wakes on half-transfer /
 * transfer-complete, runs a fourth-order CIC decimation filter, and
 * outputs signed int16 PCM samples into the zstreamer pipeline.
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

/* CIC decimation filter order.  4 is standard for PDM-to-PCM. */
#define CIC_ORDER 4

/* Number of PCM samples per half-buffer.  Each sample is int16_t.
 * Half-buffer byte size equals the graph buffer-size so that one
 * process() call fills exactly one net_buf.
 */
#define HALF_PCM_SAMPLES(inst)                                                 \
  (DT_PROP(DT_PARENT(DT_DRV_INST(inst)), buffer_size) / sizeof(int16_t))

/* Decimation ratio = PDM clock / PCM sample rate. */
#define DECIMATION_RATIO(inst)                                                 \
  (DT_INST_PROP(inst, pdm_clk_freq_hz) / DT_INST_PROP(inst, sample_rate_hz))

/* PDM bits are packed into 16-bit DMA words. */
#define BITS_PER_WORD 16

/* Number of 16-bit PDM words per half-buffer. */
#define HALF_PDM_WORDS(inst)                                                   \
  ((HALF_PCM_SAMPLES(inst) * DECIMATION_RATIO(inst)) / BITS_PER_WORD)

#define TOTAL_PDM_WORDS(inst) (HALF_PDM_WORDS(inst) * 2)

/* ── Structures ──────────────────────────────────────────────────── */

struct pdm_src_config {
  struct zstreamer_source_config common;
  SAI_Block_TypeDef *sai_block;
  SAI_TypeDef *sai_global;
  struct stm32_pclken sai_pclken;
  struct stm32_pclken sai_pclken_src;
  const struct device *dma_dev;
  uint32_t dma_channel;
  uint32_t dma_slot;
  uint32_t sample_rate_hz;
  uint32_t pdm_clk_freq_hz;
  bool right_channel;
  uint16_t half_pdm_words;
  uint16_t decimation_ratio;
  const struct pinctrl_dev_config *pcfg;
};

struct pdm_src_data {
  struct zstreamer_source_data common;
  struct k_sem half_ready;
  volatile uint8_t ready_half;
  uint16_t *dma_buf;
  struct dma_config dma_cfg;
  struct dma_block_config dma_blk;
  /* CIC filter state (persists across process() calls). */
  int64_t integrators[CIC_ORDER];
  int64_t comb_delay[CIC_ORDER];
  uint16_t decimation_counter;
  uint8_t cic_shift;
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
  }
}

/* ── CIC shift computation ──────────────────────────────────────── */

static uint8_t compute_cic_shift(uint32_t decimation_ratio) {
  /* CIC gain = R^M.  Compute ceil(log2(R)) * M - 15 to scale output
   * into the int16 range.
   */
  uint8_t log2_r = 0;
  uint32_t r = decimation_ratio - 1;

  while (r > 0) {
    log2_r++;
    r >>= 1;
  }

  int shift = CIC_ORDER * log2_r - 15;
  return (shift > 0) ? (uint8_t)shift : 0;
}

/* ── Source process (called in source thread loop) ────────────────── */

static int pdm_src_process(const struct device *dev, struct net_buf *buf) {
  const struct pdm_src_config *cfg = dev->config;
  struct pdm_src_data *data = dev->data;

  if (k_sem_take(&data->half_ready, K_MSEC(2000)) != 0) {
    LOG_WRN("PDM DMA half-buffer timeout");
    return -EAGAIN;
  }

  uint16_t *src = &data->dma_buf[data->ready_half * cfg->half_pdm_words];
  size_t half_bytes = cfg->half_pdm_words * sizeof(uint16_t);

  sys_cache_data_invd_range(src, half_bytes);

  uint16_t half_pcm =
      cfg->half_pdm_words * BITS_PER_WORD / cfg->decimation_ratio;
  int16_t *dst = (int16_t *)net_buf_add(buf, half_pcm * sizeof(int16_t));
  uint16_t pcm_idx = 0;

  for (uint16_t w = 0; w < cfg->half_pdm_words; w++) {
    uint16_t word = src[w];

    /* Process MSB first (bit 15 = earliest clock cycle). */
    for (int bit = BITS_PER_WORD - 1; bit >= 0; bit--) {
      int64_t x = ((word >> bit) & 1) ? 1 : -1;

      /* Integrator cascade. */
      data->integrators[0] += x;
      for (int k = 1; k < CIC_ORDER; k++) {
        data->integrators[k] += data->integrators[k - 1];
      }

      data->decimation_counter++;
      if (data->decimation_counter >= cfg->decimation_ratio) {
        data->decimation_counter = 0;

        /* Comb cascade. */
        int64_t val = data->integrators[CIC_ORDER - 1];
        for (int k = 0; k < CIC_ORDER; k++) {
          int64_t prev = data->comb_delay[k];
          data->comb_delay[k] = val;
          val -= prev;
        }

        if (pcm_idx < half_pcm) {
          dst[pcm_idx++] = (int16_t)(val >> data->cic_shift);
        }
      }
    }
  }

  return 0;
}

/* ── SAI clock setup ─────────────────────────────────────────────── */

static int pdm_src_clock_init(const struct pdm_src_config *cfg,
                              uint32_t *sai_ker_clk) {
  const struct device *clk = DEVICE_DT_GET(STM32_CLOCK_CONTROL_NODE);
  int ret;

  ret = clock_control_on(clk, (clock_control_subsys_t)&cfg->sai_pclken);
  if (ret < 0) {
    LOG_ERR("Failed to enable SAI bus clock: %d", ret);
    return ret;
  }

  ret = clock_control_configure(
      clk, (clock_control_subsys_t)&cfg->sai_pclken_src, NULL);
  if (ret < 0) {
    LOG_ERR("Failed to select SAI kernel clock: %d", ret);
    return ret;
  }

  ret = clock_control_get_rate(
      clk, (clock_control_subsys_t)&cfg->sai_pclken_src, sai_ker_clk);
  if (ret < 0) {
    LOG_ERR("Failed to get SAI kernel clock rate: %d", ret);
    return ret;
  }

  return 0;
}

/* ── SAI PDM register setup ──────────────────────────────────────── */

static int pdm_src_sai_init(const struct pdm_src_config *cfg,
                            uint32_t sai_ker_clk) {
  SAI_Block_TypeDef *block = cfg->sai_block;
  SAI_TypeDef *sai = cfg->sai_global;

  /* Disable block before configuration. */
  block->CR1 &= ~SAI_xCR1_SAIEN;
  while (block->CR1 & SAI_xCR1_SAIEN) {
  }

  /* Flush FIFO. */
  block->CR2 = SAI_xCR2_FFLUSH;

  /* Enable PDM mode with clock 1 output, 1 mic pair. */
  sai->PDMCR = SAI_PDMCR_PDMEN | SAI_PDMCR_CKEN1;
  sai->PDMDLY = 0;

  /* Compute MCKDIV: PDM_CLK = SAI_CK / (MCKDIV * 2). */
  uint32_t mckdiv = sai_ker_clk / (cfg->pdm_clk_freq_hz * 2);
  uint32_t actual_pdm_clk = sai_ker_clk / (mckdiv * 2);

  if (mckdiv == 0 || mckdiv > 63) {
    LOG_ERR("MCKDIV out of range: %u (sai_clk=%u, pdm_clk=%u)", mckdiv,
            sai_ker_clk, cfg->pdm_clk_freq_hz);
    return -EINVAL;
  }

  /* CR1: Master RX, free protocol, 16-bit data, mono, NODIV.
   * NODIV is required in PDM mode so that the internal bit-clock (SCK)
   * runs at the same rate as MCLK (the PDM clock output on CK1).
   * Without NODIV the SAI inserts an extra 256/(FRL+1) divider between
   * MCLK and SCK, under-sampling the PDM bitstream.
   */
  block->CR1 = SAI_xCR1_MODE_0           /* Master RX (0b01) */
               | (4U << SAI_xCR1_DS_Pos) /* 16-bit data (0b100) */
               | SAI_xCR1_MONO           /* Mono mode */
               | SAI_xCR1_NODIV          /* SCK = MCLK for PDM */
               | (mckdiv << SAI_xCR1_MCKDIV_Pos) |
               (cfg->right_channel ? SAI_xCR1_CKSTR : 0) | SAI_xCR1_DMAEN;

  /* CR2: FIFO threshold = 1/4 full. */
  block->CR2 = SAI_xCR2_FTH_0;

  /* Frame: 16 bits total, frame sync active for 1 bit. */
  block->FRCR = (15U << SAI_xFRCR_FRL_Pos) | SAI_xFRCR_FSDEF;

  /* Slot: 1 slot of 16 bits, slot 0 active. */
  block->SLOTR = (1U << SAI_xSLOTR_SLOTEN_Pos) | SAI_xSLOTR_SLOTSZ_0;

  LOG_INF("SAI PDM: ker_clk=%u mckdiv=%u -> pdm_clk=%u Hz (target %u)",
          sai_ker_clk, mckdiv, actual_pdm_clk, cfg->pdm_clk_freq_hz);

  return 0;
}

/* ── DMA setup ───────────────────────────────────────────────────── */

static int pdm_src_dma_init(const struct device *dev) {
  const struct pdm_src_config *cfg = dev->config;
  struct pdm_src_data *data = dev->data;
  uint32_t sai_dr_addr = (uint32_t)&cfg->sai_block->DR;

  memset(&data->dma_blk, 0, sizeof(data->dma_blk));
  data->dma_blk.source_address = sai_dr_addr;
  data->dma_blk.dest_address = (uint32_t)data->dma_buf;
  data->dma_blk.block_size = cfg->half_pdm_words * 2 * sizeof(uint16_t);
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

/* ── Start: DMA → SAI enable ─────────────────────────────────────── */

static int pdm_src_hw_start(const struct device *dev) {
  const struct pdm_src_config *cfg = dev->config;

  int ret = dma_start(cfg->dma_dev, cfg->dma_channel);
  if (ret < 0) {
    LOG_ERR("DMA start failed: %d", ret);
    return ret;
  }

  cfg->sai_block->CR1 |= SAI_xCR1_SAIEN;

  LOG_INF("PDM source started (decimation=%u, cic_shift=%u)",
          cfg->decimation_ratio, ((struct pdm_src_data *)dev->data)->cic_shift);
  return 0;
}

/* ── Device init ─────────────────────────────────────────────────── */

static int pdm_src_init(const struct device *dev) {
  const struct pdm_src_config *cfg = dev->config;
  struct pdm_src_data *data = dev->data;
  int ret;

  k_sem_init(&data->half_ready, 0, 1);
  memset(data->integrators, 0, sizeof(data->integrators));
  memset(data->comb_delay, 0, sizeof(data->comb_delay));
  data->decimation_counter = 0;
  data->cic_shift = compute_cic_shift(cfg->decimation_ratio);

  ret = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
  if (ret < 0) {
    LOG_ERR("Failed to apply pinctrl: %d", ret);
    return ret;
  }

  uint32_t sai_ker_clk;

  ret = pdm_src_clock_init(cfg, &sai_ker_clk);
  if (ret < 0) {
    return ret;
  }

  ret = pdm_src_sai_init(cfg, sai_ker_clk);
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

/* SAI global (SAI_TypeDef) is 4 bytes before Block A (SAI_Block_TypeDef). */
#define SAI_BLOCK_REG(inst)                                                    \
  ((SAI_Block_TypeDef *)DT_REG_ADDR(DT_INST_PHANDLE(inst, sai)))

#define SAI_GLOBAL_REG(inst)                                                   \
  ((SAI_TypeDef *)(DT_REG_ADDR(DT_INST_PHANDLE(inst, sai)) - 0x04))

#define SAI_PCLKEN(inst)                                                       \
  {                                                                            \
      .bus = DT_CLOCKS_CELL_BY_IDX(DT_INST_PHANDLE(inst, sai), 0, bus),        \
      .enr = DT_CLOCKS_CELL_BY_IDX(DT_INST_PHANDLE(inst, sai), 0, bits),       \
  }

#define SAI_PCLKEN_SRC(inst)                                                   \
  {                                                                            \
      .bus = DT_CLOCKS_CELL_BY_IDX(DT_INST_PHANDLE(inst, sai), 1, bus),        \
      .enr = DT_CLOCKS_CELL_BY_IDX(DT_INST_PHANDLE(inst, sai), 1, bits),       \
  }

#define PDM_SRC_DEFINE(inst)                                                   \
  BUILD_ASSERT(                                                                \
      DT_INST_PROP(inst, pdm_clk_freq_hz) %                                    \
              DT_INST_PROP(inst, sample_rate_hz) ==                            \
          0,                                                                   \
      "pdm-clk-freq-hz must be an integer multiple of sample-rate-hz");        \
                                                                               \
  BUILD_ASSERT(DECIMATION_RATIO(inst) <= 215,                                  \
               "decimation ratio too large for CIC order 4 with int64");       \
                                                                               \
  BUILD_ASSERT(HALF_PDM_WORDS(inst) * BITS_PER_WORD ==                         \
                   HALF_PCM_SAMPLES(inst) * DECIMATION_RATIO(inst),            \
               "half-buffer PDM bits must be exact multiple of 16");           \
                                                                               \
  ZSTREAMER_SOURCE_DT_INST_PRE_DEFINE(inst);                                   \
  PINCTRL_DT_INST_DEFINE(inst);                                                \
                                                                               \
  static uint16_t pdm_dma_buf_##inst[TOTAL_PDM_WORDS(inst)] __aligned(32)      \
      __attribute__((section(".noinit")));                                     \
                                                                               \
  static struct pdm_src_data pdm_src_data_##inst = {                           \
      .common = ZSTREAMER_SOURCE_DATA_INIT(inst),                              \
      .dma_buf = pdm_dma_buf_##inst,                                           \
  };                                                                           \
                                                                               \
  static const struct pdm_src_config pdm_src_config_##inst = {                 \
      .common = ZSTREAMER_SOURCE_CONFIG_INIT(inst),                            \
      .sai_block = SAI_BLOCK_REG(inst),                                        \
      .sai_global = SAI_GLOBAL_REG(inst),                                      \
      .sai_pclken = SAI_PCLKEN(inst),                                          \
      .sai_pclken_src = SAI_PCLKEN_SRC(inst),                                  \
      .dma_dev = DEVICE_DT_GET(DT_INST_DMAS_CTLR_BY_NAME(inst, rx)),           \
      .dma_channel = DT_INST_DMAS_CELL_BY_NAME(inst, rx, channel),             \
      .dma_slot = DT_INST_DMAS_CELL_BY_NAME(inst, rx, slot),                   \
      .sample_rate_hz = DT_INST_PROP(inst, sample_rate_hz),                    \
      .pdm_clk_freq_hz = DT_INST_PROP(inst, pdm_clk_freq_hz),                  \
      .right_channel = DT_INST_PROP(inst, right_channel),                      \
      .decimation_ratio = DECIMATION_RATIO(inst),                              \
      .half_pdm_words = HALF_PDM_WORDS(inst),                                  \
      .pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(inst),                            \
  };                                                                           \
                                                                               \
  DEVICE_DT_INST_DEFINE(inst, pdm_src_init, NULL, &pdm_src_data_##inst,        \
                        &pdm_src_config_##inst, POST_KERNEL,                   \
                        CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &pdm_src_api);

DT_INST_FOREACH_STATUS_OKAY(PDM_SRC_DEFINE)
