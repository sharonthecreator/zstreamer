/*
 * Copyright (c) 2026 zstreamer contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zstreamer/zstreamer.h>

LOG_MODULE_REGISTER(zstreamer_sample, LOG_LEVEL_INF);

int main(void)
{
	LOG_INF("zstreamer basic sample");

	return 0;
}
