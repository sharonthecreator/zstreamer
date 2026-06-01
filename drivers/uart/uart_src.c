/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT zstreamer_uart_src

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>

#include <zstreamer/source.h>

#if defined(CONFIG_UART_ASYNC_API)
#include "uart_dma_context.h"
#endif

LOG_MODULE_REGISTER(uart_src, CONFIG_ZSTREAMER_LOG_LEVEL);

#if defined(CONFIG_UART_ASYNC_API)
#ifndef CONFIG_ZSTREAMER_UART_DMA_RX_BUF_SIZE
#define CONFIG_ZSTREAMER_UART_DMA_RX_BUF_SIZE 256
#endif
#endif

#if defined(CONFIG_UART_INTERRUPT_DRIVEN)
#define UART_SRC_IRQ_BUF_SIZE 32
#endif

struct uart_src_config {
	struct zstreamer_source_config common;
	const struct device *uart_dev;
};

struct uart_src_data {
	struct zstreamer_source_data common;
#if defined(CONFIG_UART_ASYNC_API)
	uint8_t dma_rx_buf[CONFIG_ZSTREAMER_UART_DMA_RX_BUF_SIZE];
	struct k_sem rx_sem;
	const uint8_t *rx_data;
	size_t rx_len;
	bool dma_enabled;
#endif
#if defined(CONFIG_UART_INTERRUPT_DRIVEN)
	uint8_t irq_buf[UART_SRC_IRQ_BUF_SIZE];
	volatile uint8_t irq_len;
	struct k_sem irq_sem;
	bool irq_enabled;
#endif
};

#if defined(CONFIG_UART_ASYNC_API)

static void uart_src_rx_handler(void *user_data, const uint8_t *data, size_t len)
{
	struct uart_src_data *drv_data = user_data;

	drv_data->rx_data = data;
	drv_data->rx_len = len;
	k_sem_give(&drv_data->rx_sem);
}

static int uart_src_open_dma(const struct device *dev)
{
	const struct uart_src_config *cfg = dev->config;
	struct uart_src_data *data = dev->data;
	int ret;

	ret = uart_dma_context_register_rx(cfg->uart_dev, uart_src_rx_handler, data);
	if (ret != 0) {
		LOG_ERR("Failed to register RX handler: %d", ret);
		return ret;
	}

	ret = uart_dma_context_rx_enable(cfg->uart_dev, data->dma_rx_buf, sizeof(data->dma_rx_buf),
					 SYS_FOREVER_US);
	if (ret != 0) {
		LOG_ERR("Failed to enable RX: %d", ret);
		uart_dma_context_unregister_rx(cfg->uart_dev);
		return ret;
	}

	data->dma_enabled = true;
	LOG_DBG("DMA RX enabled");
	return 0;
}

static int uart_src_process_dma(const struct device *dev, struct net_buf *buf)
{
	struct uart_src_data *data = dev->data;

	/* Wait for RX data with timeout. */
	if (k_sem_take(&data->rx_sem, K_MSEC(100)) != 0) {
		return -EAGAIN;
	}

	if (data->rx_len == 0) {
		return -EAGAIN;
	}

	size_t copy_len = MIN(data->rx_len, net_buf_tailroom(buf));

	net_buf_add_mem(buf, data->rx_data, copy_len);

	return 0;
}

#endif /* CONFIG_UART_ASYNC_API */

#if defined(CONFIG_UART_INTERRUPT_DRIVEN)

static void uart_src_irq_handler(const struct device *uart_dev, void *user_data)
{
	struct uart_src_data *data = user_data;

	/* Clear error flags (ORE/FE/NE/PE) before checking RXNE.
	 * RXNEIE triggers interrupts on ORE too — uncleared ORE
	 * causes an infinite ISR loop. */
	uart_err_check(uart_dev);
	uart_irq_update(uart_dev);

	if (!uart_irq_rx_ready(uart_dev)) {
		return;
	}

	int space = UART_SRC_IRQ_BUF_SIZE - data->irq_len;

	if (space > 0) {
		int n = uart_fifo_read(uart_dev, data->irq_buf + data->irq_len, space);

		if (n > 0) {
			data->irq_len += n;
			k_sem_give(&data->irq_sem);
		}
	} else {
		/* Buffer full — drain to prevent UART overrun. */
		uint8_t discard;

		while (uart_fifo_read(uart_dev, &discard, 1) > 0) {
		}
		LOG_WRN("IRQ buffer overflow, bytes discarded");
	}
}

static int uart_src_open_irq(const struct device *dev)
{
	const struct uart_src_config *cfg = dev->config;
	struct uart_src_data *data = dev->data;

	k_sem_init(&data->irq_sem, 0, 1);
	uart_irq_callback_user_data_set(cfg->uart_dev, uart_src_irq_handler, data);
	uart_irq_rx_enable(cfg->uart_dev);
	data->irq_enabled = true;

	LOG_INF("IRQ RX enabled for %s", cfg->uart_dev->name);
	return 0;
}

static int uart_src_process_irq(const struct device *dev, struct net_buf *buf)
{
	struct uart_src_data *data = dev->data;
	uint16_t target = net_buf_tailroom(buf);

	/* Block until at least one byte is buffered by the ISR. */
	if (data->irq_len == 0) {
		if (k_sem_take(&data->irq_sem, K_MSEC(100)) != 0) {
			return -EAGAIN;
		}
	}

	/* Wait for target bytes with inter-byte timeout (100 ms). */
	int idle_polls = 0;
	const int max_idle = 2000; /* 2000 * 50 us = 100 ms */

	while (data->irq_len < target) {
		k_busy_wait(50);
		if (++idle_polls > max_idle) {
			break;
		}
	}

	/* Copy buffered bytes to net_buf under IRQ lock. */
	unsigned int key = irq_lock();
	size_t len = MIN(data->irq_len, net_buf_tailroom(buf));

	if (len > 0) {
		net_buf_add_mem(buf, data->irq_buf, len);
		/* Shift any remaining bytes forward. */
		if (data->irq_len > len) {
			memmove(data->irq_buf, data->irq_buf + len, data->irq_len - len);
		}
		data->irq_len -= len;
	}
	k_sem_reset(&data->irq_sem);
	if (data->irq_len > 0) {
		k_sem_give(&data->irq_sem);
	}
	irq_unlock(key);

	if (len == 0) {
		return -EAGAIN;
	}

	if (len < target) {
		LOG_WRN("Short message: got %zu/%u bytes", len, target);
	}

	return 0;
}

#endif /* CONFIG_UART_INTERRUPT_DRIVEN */

static int uart_src_process_poll(const struct device *dev, struct net_buf *buf)
{
	const struct uart_src_config *cfg = dev->config;
	unsigned char c;
	uint16_t target = net_buf_tailroom(buf);

	/* Clear any UART error flags (overrun, framing, etc.) before polling.
	 * On STM32WL55 the UART can enter a broken state after LoRa TX;
	 * reading the error register clears the flags and recovers the UART. */
	int err_flags = uart_err_check(cfg->uart_dev);

	if (err_flags) {
		LOG_WRN("UART errors cleared: 0x%02x", err_flags);
	}

	/* Accumulate bytes until we reach the target message size.
	 * Poll with k_busy_wait(50 us) between attempts to avoid UART overrun
	 * (single-byte RX register, no FIFO in polling mode).
	 * Allow up to 100 ms of idle time between bytes so that upstream
	 * pipeline latency (gpio_pulse delays, poll_out gaps) does not cause
	 * premature short reads. */
	int idle_polls = 0;
	const int max_idle = 2000; /* 2000 * 50 us = 100 ms */

	while (buf->len < target) {
		if (uart_poll_in(cfg->uart_dev, &c) < 0) {
			if (buf->len == 0) {
				/* Nothing received yet — short sleep and retry. */
				k_msleep(1);
				return -EAGAIN;
			}
			idle_polls++;
			if (idle_polls > max_idle) {
				LOG_WRN("Short message: got %u/%u bytes", buf->len, target);
				break;
			}
			k_busy_wait(50);
			continue;
		}
		idle_polls = 0;
		net_buf_add_u8(buf, c);
	}

	if (buf->len == 0) {
		return -EAGAIN;
	}

	return 0;
}

static int uart_src_process(const struct device *dev, struct net_buf *buf)
{
	struct uart_src_data *data = dev->data;
	int ret;

	ARG_UNUSED(data);

#if defined(CONFIG_UART_ASYNC_API)
	if (data->dma_enabled) {
		ret = uart_src_process_dma(dev, buf);
	} else
#endif
#if defined(CONFIG_UART_INTERRUPT_DRIVEN)
	if (data->irq_enabled) {
		ret = uart_src_process_irq(dev, buf);
	} else
#endif
	{
		ret = uart_src_process_poll(dev, buf);
	}

	if (ret == 0 && buf->len > 0) {
		LOG_DBG("uart_src RX %u bytes", buf->len);
		LOG_HEXDUMP_DBG(buf->data, buf->len, "uart_src RX data");
	}

	return ret;
}

static const struct zstreamer_node_driver_api uart_src_api = {
	.process = uart_src_process,
};

static int uart_src_init(const struct device *dev)
{
	const struct uart_src_config *cfg = dev->config;
	bool opened = false;

#if defined(CONFIG_UART_ASYNC_API)
	{
		struct uart_src_data *data = dev->data;

		k_sem_init(&data->rx_sem, 0, 1);

		/* Try to enable DMA first. */
		if (uart_src_open_dma(dev) == 0) {
			opened = true;
		} else {
			LOG_INF("DMA not available for %s", cfg->uart_dev->name);
		}
	}
#endif
#if defined(CONFIG_UART_INTERRUPT_DRIVEN)
	if (!opened) {
		if (uart_src_open_irq(dev) == 0) {
			opened = true;
		} else {
			LOG_WRN("IRQ RX setup failed for %s", cfg->uart_dev->name);
		}
	}
#endif
	if (!opened) {
		LOG_INF("%s: using polling mode", cfg->uart_dev->name);
	}

	return zstreamer_source_common_init(dev);
}

#define UART_SRC_DEFINE(inst)                                                                      \
	ZSTREAMER_SOURCE_DT_INST_PRE_DEFINE(inst);                                                 \
	static struct uart_src_data uart_src_data_##inst = {                                       \
		.common = ZSTREAMER_SOURCE_DATA_INIT(inst),                                        \
	};                                                                                         \
	static const struct uart_src_config uart_src_config_##inst = {                             \
		.common = ZSTREAMER_SOURCE_CONFIG_INIT(inst),                                      \
		.uart_dev = DEVICE_DT_GET(DT_INST_PHANDLE(inst, uart_device)),                     \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, uart_src_init, NULL, &uart_src_data_##inst,                    \
			      &uart_src_config_##inst, POST_KERNEL,                                \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &uart_src_api);

DT_INST_FOREACH_STATUS_OKAY(UART_SRC_DEFINE)
