/*
 * Copyright (c) 2026 sharonthecreator
 * SPDX-License-Identifier: Apache-2.0
 *
 * Filesystem mount gate for zstreamer.
 *
 * Lazily checks whether the filesystem is mounted on the first
 * buffer — not in init, because fstab automount runs at POST_KERNEL
 * priority 99 (after device init).  If mounted, passes all subsequent
 * buffers to children; otherwise drops them forever.
 */

#define DT_DRV_COMPAT zstreamer_fs_mounted_filter

#include <zephyr/device.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>

#include <zstreamer/filter.h>

LOG_MODULE_REGISTER(fs_mounted_filter, CONFIG_ZSTREAMER_LOG_LEVEL);

struct fs_mounted_filter_config {
	struct zstreamer_filter_config common;
	const char *mount_path;
};

struct fs_mounted_filter_data {
	struct zstreamer_filter_data common;
	bool checked;
	bool fs_available;
};

static int fs_mounted_filter_process(const struct device *dev, struct net_buf *buf)
{
	struct fs_mounted_filter_data *data = dev->data;

	ARG_UNUSED(buf);

	if (!data->checked) {
		const struct fs_mounted_filter_config *cfg = dev->config;
		struct fs_dir_t dir;

		fs_dir_t_init(&dir);
		int rc = fs_opendir(&dir, cfg->mount_path);
		if (rc == 0) {
			fs_closedir(&dir);
		}
		data->fs_available = (rc == 0);
		data->checked = true;
		if (data->fs_available) {
			LOG_INF("%s mounted, passing buffers", cfg->mount_path);
		} else {
			LOG_WRN("%s not mounted, dropping all buffers", cfg->mount_path);
		}
	}

	return data->fs_available ? 1 : 0;
}

static int fs_mounted_filter_init(const struct device *dev)
{
	struct fs_mounted_filter_data *data = dev->data;

	data->checked = false;
	data->fs_available = false;

	return zstreamer_filter_common_init(dev);
}

static const struct zstreamer_node_driver_api fs_mounted_filter_api = {
	.process = fs_mounted_filter_process,
};

#define FS_MOUNTED_FILTER_DEFINE(inst)                                                             \
	ZSTREAMER_FILTER_DT_INST_PRE_DEFINE(inst);                                                 \
	static struct fs_mounted_filter_data fs_mounted_filter_data_##inst = {                     \
		.common = ZSTREAMER_FILTER_DATA_INIT(inst),                                        \
	};                                                                                         \
	static const struct fs_mounted_filter_config fs_mounted_filter_config_##inst = {           \
		.common = ZSTREAMER_FILTER_CONFIG_INIT(inst, true),                                \
		.mount_path = DT_INST_PROP(inst, mount_path),                                      \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, fs_mounted_filter_init, NULL, &fs_mounted_filter_data_##inst,  \
			      &fs_mounted_filter_config_##inst, POST_KERNEL,                       \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &fs_mounted_filter_api);

DT_INST_FOREACH_STATUS_OKAY(FS_MOUNTED_FILTER_DEFINE)
